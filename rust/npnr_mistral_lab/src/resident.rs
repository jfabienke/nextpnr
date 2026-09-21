// SPDX-License-Identifier: ISC
//! Resident LAB snapshots, patched one bel at a time.
//!
//! The capture path validates and evaluates a whole LAB record per query. On a
//! large design the legaliser asks tens of millions of times and building that
//! record was most of the cost. A resident snapshot keeps every LAB's facts
//! here and takes, per query, only the bels the caller changed since its last
//! query: the placer binds one candidate, asks, and usually unbinds it, so a
//! query carries one LUT or one register. A patch is a trial (in view for this
//! evaluation, never applied) or a commit. The ALM input counts and the LAB
//! total are kept per ALM, and the control-set rules run on a resident mirror
//! of the control model's own snapshot, so a register query costs the rules
//! and not a projection. Net ids are caller-chosen keys, nonzero and stable
//! for the run; the rules compare ids for equality only, and the mirror maps
//! the control keys to the model's small ids with a reference-counted table.
//! The verdict equals `evaluate_lab_v2` over the same facts field for field.
//!
//! What the arch sends in one call, and what the oracle test must generate
//! (`mistral/lab_resident.cc` is the caller):
//! 1. one trial: the legaliser bound a candidate and asks;
//! 2. nothing: the bel is back to the committed facts, or the LAB is unchanged;
//! 3. commits: bels found unchanged at the query after their trial;
//! 4. a commit and a trial of the same ALM in one call;
//! 5. several trials, in one ALM or several: the annealer's swaps;
//! 6. a burst of up to sixty commits: a LAB just reset, or a chain bound
//!    before any query (more than `MAX_TRIALS` changed bels);
//! 7. a patch whose facts equal the held facts, which is skipped;
//! 8. a scan (`evaluate_scan`): any of the shapes above to bring the LAB up to
//!    date, then one candidate's facts and the free bels the legaliser would
//!    try, in its order; the first bel the rules accept comes back. The
//!    candidate is held in view at each bel as a trial would be and never
//!    applied, so a scan costs the refused bels no bind and no patch.

use crate::model::{ControlLabSnapshot, ControlSignal, NetId};
use crate::rules::{ControlAssessment, evaluate as evaluate_controls};
use crate::v2::{
    AlmView, Discard, check_alm, check_mlab, evaluate_facts, ff_shape_valid, lut_shape_valid,
    query_valid, recompute_inputs, reject,
};
use crate::v2_wire::*;
use crate::wire::{CONTROL_COUNT, FF_COUNT, GLOBAL, INVERTED, LEGAL, MAX_NETS};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ResidentError {
    /// The LAB index is outside the session.
    Lab,
    /// The LAB was never reset.
    Uninitialised,
    /// The query scope is malformed.
    Query,
    /// A patch fails the shape rules, names a bel twice, would need more
    /// distinct control nets than the control model holds, or more than
    /// `MAX_TRIALS` trials travel together; nothing was applied.
    Patch,
}

/// Trials in view per query: the placer changes a bel or two between queries;
/// a resync after a reset travels as commits.
pub const MAX_TRIALS: usize = 8;

/// The control model's view of the LAB, kept current with the committed facts:
/// the model's own snapshot (one row per register, and each id's global
/// class), plus the table that maps control net keys to the model's ids. A
/// trial's rows are written into the snapshot for one evaluation and undone.
struct ControlMirror {
    snapshot: ControlLabSnapshot,
    keys: [u32; MAX_NETS + 1],
    refs: [u16; MAX_NETS + 1],
    /// The highest id in use; the tables are scanned only that far.
    len: usize,
}

/// What a trial changed in the snapshot, for the undo.
struct TrialUndo {
    rows: [(usize, Option<[ControlSignal; CONTROL_COUNT]>); MAX_TRIALS],
    row_count: usize,
    temp_ids: usize,
}

impl Clone for ControlMirror {
    fn clone(&self) -> Self {
        Self {
            snapshot: self.snapshot.duplicate(),
            keys: self.keys,
            refs: self.refs,
            len: self.len,
        }
    }
}

impl ControlMirror {
    fn empty() -> Self {
        Self {
            snapshot: ControlLabSnapshot::from_parts([None; FF_COUNT], [0; MAX_NETS + 1]),
            keys: [0; MAX_NETS + 1],
            refs: [0; MAX_NETS + 1],
            len: 0,
        }
    }

    fn find(&self, key: u32) -> Option<usize> {
        (1..=self.len).find(|&id| self.refs[id] != 0 && self.keys[id] == key)
    }

    /// The id for a key, taking a reference; `None` when the table is full or
    /// the key was seen with the other global class (a net is global or not,
    /// so the arch never sends that; the capture path reports it as a boundary
    /// error, this path as a rejected patch).
    fn acquire(&mut self, key: u32, global: bool) -> Option<usize> {
        if let Some(id) = self.find(key) {
            if (self.snapshot.classes[id] == 2) != global {
                return None;
            }
            self.refs[id] += 1;
            return Some(id);
        }
        let id = (1..=self.len)
            .find(|&id| self.refs[id] == 0)
            .or((self.len < MAX_NETS).then_some(self.len + 1))?;
        self.len = self.len.max(id);
        self.keys[id] = key;
        self.refs[id] = 1;
        self.snapshot.classes[id] = if global { 2 } else { 1 };
        Some(id)
    }

    fn release(&mut self, id: usize) {
        self.refs[id] -= 1;
        if self.refs[id] == 0 {
            self.snapshot.classes[id] = 0;
            self.keys[id] = 0;
            while self.len > 0 && self.refs[self.len] == 0 {
                self.len -= 1;
            }
        }
    }

    fn row_of(signals: &[ControlSignal; CONTROL_COUNT]) -> [Option<usize>; CONTROL_COUNT] {
        signals.map(|s| s.net_index())
    }

    /// Replaces register `row` with the facts of `ff`; `None` when the table is full.
    fn set(&mut self, row: usize, ff: &LabFfV2) -> Option<()> {
        if let Some(old) = self.snapshot.ffs[row] {
            for id in Self::row_of(&old).into_iter().flatten() {
                self.release(id);
            }
            self.snapshot.ffs[row] = None;
        }
        if ff.occupied == 0 {
            return Some(());
        }
        let mut signals = [ControlSignal::EMPTY; CONTROL_COUNT];
        for (kind, raw) in ff.control.iter().enumerate() {
            if raw.net_id == 0 {
                continue;
            }
            let Some(id) = self.acquire(raw.net_id, raw.flags & GLOBAL != 0) else {
                for signal in signals.iter().take(kind) {
                    if let Some(id) = signal.net_index() {
                        self.release(id);
                    }
                }
                return None;
            };
            signals[kind] =
                ControlSignal::new(NetId::from_index(id as u16), raw.flags & INVERTED != 0);
        }
        self.snapshot.ffs[row] = Some(signals);
        Some(())
    }

    /// Writes the trial rows into the snapshot: known control nets keep their
    /// ids, new ones take ids past the table for this evaluation. `None` when
    /// they would not fit or a key changes class; nothing is written then.
    fn apply_trials(&mut self, rows: &[(usize, &LabFfV2)]) -> Option<TrialUndo> {
        let mut undo = TrialUndo {
            rows: [(0, None); MAX_TRIALS],
            row_count: 0,
            temp_ids: 0,
        };
        let mut temp_keys = [0u32; MAX_TRIALS * CONTROL_COUNT];
        let mut new_rows: [Option<[ControlSignal; CONTROL_COUNT]>; MAX_TRIALS] = [None; MAX_TRIALS];
        for (i, &(row, ff)) in rows.iter().enumerate() {
            undo.rows[i] = (row, self.snapshot.ffs[row]);
            undo.row_count = i + 1;
            if ff.occupied == 0 {
                continue;
            }
            let mut signals = [ControlSignal::EMPTY; CONTROL_COUNT];
            for (kind, raw) in ff.control.iter().enumerate() {
                if raw.net_id == 0 {
                    continue;
                }
                let global = raw.flags & GLOBAL != 0;
                let id = match self.find(raw.net_id) {
                    Some(id) => {
                        if (self.snapshot.classes[id] == 2) != global {
                            return None;
                        }
                        id
                    }
                    None => {
                        let index = match temp_keys[..undo.temp_ids]
                            .iter()
                            .position(|&k| k == raw.net_id)
                        {
                            Some(index) => index,
                            None => {
                                if undo.temp_ids == temp_keys.len()
                                    || self.len + undo.temp_ids + 1 > MAX_NETS
                                {
                                    return None;
                                }
                                temp_keys[undo.temp_ids] = raw.net_id;
                                undo.temp_ids += 1;
                                undo.temp_ids - 1
                            }
                        };
                        let id = self.len + 1 + index;
                        // Ids past the table are unused: their class is free to set.
                        if temp_keys[index] == raw.net_id
                            && index < undo.temp_ids
                            && self.snapshot.classes[id] != 0
                            && (self.snapshot.classes[id] == 2) != global
                        {
                            return None;
                        }
                        id
                    }
                };
                signals[kind] =
                    ControlSignal::new(NetId::from_index(id as u16), raw.flags & INVERTED != 0);
            }
            new_rows[i] = Some(signals);
        }
        // Everything checked: write the classes of the temporary ids and the rows.
        for (index, &key) in temp_keys[..undo.temp_ids].iter().enumerate() {
            let global = rows.iter().any(|(_, ff)| {
                ff.occupied != 0
                    && ff
                        .control
                        .iter()
                        .any(|raw| raw.net_id == key && raw.flags & GLOBAL != 0)
            });
            self.snapshot.classes[self.len + 1 + index] = if global { 2 } else { 1 };
        }
        for (i, &(row, _)) in rows.iter().enumerate() {
            self.snapshot.ffs[row] = new_rows[i];
        }
        Some(undo)
    }

    fn undo_trials(&mut self, undo: &TrialUndo) {
        for &(row, old) in &undo.rows[..undo.row_count] {
            self.snapshot.ffs[row] = old;
        }
        for index in 0..undo.temp_ids {
            self.snapshot.classes[self.len + 1 + index] = 0;
        }
    }
}

impl ControlSignal {
    fn net_index(self) -> Option<usize> {
        self.net_id_u16().map(usize::from)
    }
}

#[derive(Clone)]
struct ResidentLab {
    facts: LabFactsV2,
    /// Which of the sixty bels the committed facts occupy (bit `alm * 6 + slot`), kept beside
    /// the facts so a scan decides "free" with a bit test instead of reading forty records.
    occupied: u64,
    alm_inputs: [i32; ALMS],
    total_inputs: i32,
    mirror: ControlMirror,
    initialised: bool,
}

pub struct ResidentLabs {
    labs: Vec<ResidentLab>,
    input_limit: i32,
}

/// The control verdict fields as the wire result reports them.
struct ControlVerdict {
    status: u32,
    reason: u32,
    kind: u32,
    ff_slot: u32,
    mask: u32,
}

fn control_verdict(snapshot: &ControlLabSnapshot) -> ControlVerdict {
    match evaluate_controls(snapshot) {
        ControlAssessment::Legal(_) => ControlVerdict {
            status: LEGAL,
            reason: 0,
            kind: u32::MAX,
            ff_slot: u32::MAX,
            mask: 0,
        },
        ControlAssessment::Illegal(conflict) => ControlVerdict {
            status: crate::wire::ILLEGAL,
            reason: conflict.reason() as u32,
            kind: conflict.control_kind() as u32,
            ff_slot: conflict.ff_slot(),
            mask: conflict.resource_mask(),
        },
    }
}

impl ResidentLabs {
    /// `None` when the storage cannot be reserved.
    pub fn new(lab_count: usize, input_limit: i32) -> Option<Self> {
        let mut labs = Vec::new();
        labs.try_reserve_exact(lab_count).ok()?;
        let empty = ResidentLab {
            facts: LabFactsV2 {
                input_limit,
                ..LabFactsV2::default()
            },
            occupied: 0,
            alm_inputs: [0; ALMS],
            total_inputs: 0,
            mirror: ControlMirror::empty(),
            initialised: false,
        };
        labs.resize(lab_count, empty);
        Some(Self { labs, input_limit })
    }

    pub fn lab_count(&self) -> usize {
        self.labs.len()
    }

    pub fn input_limit(&self) -> i32 {
        self.input_limit
    }

    /// Empties the LAB and records whether it is an MLAB.
    pub fn reset(&mut self, lab: usize, is_mlab: bool) -> Result<(), ResidentError> {
        let limit = self.input_limit;
        let lab = self.labs.get_mut(lab).ok_or(ResidentError::Lab)?;
        lab.facts = LabFactsV2 {
            input_limit: limit,
            is_mlab: u32::from(is_mlab),
            ..LabFactsV2::default()
        };
        lab.occupied = 0;
        lab.alm_inputs = [0; ALMS];
        lab.total_inputs = 0;
        lab.mirror = ControlMirror::empty();
        lab.initialised = true;
        Ok(())
    }

    /// The LAB's committed facts, for a caller that wants to cross-check.
    pub fn facts(&self, lab: usize) -> Result<&LabFactsV2, ResidentError> {
        let lab = self.labs.get(lab).ok_or(ResidentError::Lab)?;
        if !lab.initialised {
            return Err(ResidentError::Uninitialised);
        }
        Ok(&lab.facts)
    }

    fn patch_valid(patch: &BelPatchV2) -> bool {
        let any_key = |_: u32| true;
        if patch.alm as usize >= ALMS
            || patch.slot as usize >= LUTS + FFS
            || patch.commit > 1
            || patch.alm_inputs < 0
        {
            return false;
        }
        if (patch.slot as usize) < LUTS {
            patch.ff == LabFfV2::default() && lut_shape_valid(&patch.lut, &any_key)
        } else {
            patch.lut == LabLutV2::default() && ff_shape_valid(&patch.ff, &any_key)
        }
    }

    fn bel_changes(alm: &AlmFactsV2, patch: &BelPatchV2) -> bool {
        if (patch.slot as usize) < LUTS {
            alm.lut[patch.slot as usize] != patch.lut
        } else {
            alm.ff[patch.slot as usize - LUTS] != patch.ff
        }
    }

    fn apply_bel(alm: &mut AlmFactsV2, patch: &BelPatchV2) {
        if (patch.slot as usize) < LUTS {
            alm.lut[patch.slot as usize] = patch.lut;
        } else {
            alm.ff[patch.slot as usize - LUTS] = patch.ff;
        }
    }

    /// Applies the commits, evaluates the query with the trials in view, and
    /// returns the verdict. Patches are validated first, all or none. With
    /// `recompute` the ALM input counts are recomputed from the facts (the
    /// harness modes); without it the caller's counts are taken as facts.
    pub fn evaluate(
        &mut self,
        lab: usize,
        patches: &[BelPatchV2],
        query: u32,
        query_alm: u32,
        recompute: bool,
    ) -> Result<LabVerdictV2, ResidentError> {
        let limit = self.input_limit;
        let lab = self.labs.get_mut(lab).ok_or(ResidentError::Lab)?;
        if !lab.initialised {
            return Err(ResidentError::Uninitialised);
        }
        if !query_valid(query, query_alm) {
            return Err(ResidentError::Query);
        }
        Self::validate(patches, MAX_TRIALS)?;
        let (trials, trial_count) = Self::sync(lab, patches, recompute)?;
        Ok(Self::verdict(
            lab,
            limit,
            &trials[..trial_count],
            query,
            query_alm,
            recompute,
        ))
    }

    /// The legaliser's scan of a tile as one question (module doc, shape 8).
    /// `patches` bring the LAB up to date exactly as in `evaluate`. Then the
    /// candidate's facts (a LUT half or a register; its `alm`, `slot`, `commit`,
    /// and `alm_inputs` are ignored) are held in view at each bel of `order` in
    /// turn (`alm * 6 + slot`, free bels of the candidate's kind, no repeats)
    /// and the bel's own query is answered; the index in `order` of the first
    /// legal bel comes back, or `None`. Counts of trial ALMs are recomputed
    /// from the facts, since the caller keeps a count only for what is bound.
    /// Each answer equals `evaluate` with the candidate sent as a trial at that
    /// bel; nothing of the candidate is applied.
    pub fn evaluate_scan(
        &mut self,
        lab: usize,
        patches: &[BelPatchV2],
        candidate: &BelPatchV2,
        order: &[u8],
        recompute: bool,
    ) -> Result<Option<usize>, ResidentError> {
        let limit = self.input_limit;
        let lab = self.labs.get_mut(lab).ok_or(ResidentError::Lab)?;
        if !lab.initialised {
            return Err(ResidentError::Uninitialised);
        }
        let is_lut = candidate.lut.occupied != 0;
        let mut shaped = *candidate;
        shaped.alm = 0;
        shaped.slot = if is_lut { 0 } else { LUTS as u32 };
        shaped.commit = 0;
        shaped.alm_inputs = 0;
        let occupied = if is_lut {
            shaped.lut.occupied
        } else {
            shaped.ff.occupied
        };
        if occupied != 1 || !Self::patch_valid(&shaped) {
            return Err(ResidentError::Patch);
        }
        // One slot of the trials in view is the candidate's.
        Self::validate(patches, MAX_TRIALS - 1)?;
        // The bels taken in the state the patches describe: the committed ones, with each
        // patch's bel set or cleared. The bels of the order must be free, distinct, and of the
        // candidate's kind.
        let mut taken = lab.occupied;
        for patch in patches {
            let (bit, occupied) = Self::bel_bit(patch);
            taken = if occupied { taken | bit } else { taken & !bit };
        }
        let mut seen = 0u64;
        for &bel in order {
            let (alm, slot) = (bel as usize / (LUTS + FFS), bel as usize % (LUTS + FFS));
            let bit = 1u64.checked_shl(u32::from(bel)).unwrap_or(0);
            if alm >= ALMS || (slot < LUTS) != is_lut || (seen | taken) & bit != 0 {
                return Err(ResidentError::Query);
            }
            seen |= bit;
        }
        let (trials, trial_count) = Self::sync(lab, patches, recompute)?;
        let query = if is_lut { QUERY_COMB_BEL } else { QUERY_FF_BEL };
        let is_mlab = lab.facts.is_mlab != 0;
        let trials = &trials[..trial_count];
        let mut at = shaped;
        for (index, &bel) in order.iter().enumerate() {
            at.alm = u32::from(bel) / (LUTS + FFS) as u32;
            at.slot = u32::from(bel) % (LUTS + FFS) as u32;
            if !is_lut && Self::second_register_bel(at.slot as usize) {
                // More than half the bels a scan is asked about on a crowded LAB: the ALM rule
                // refuses a register there whatever else the ALM holds (`check_alm`, ODD_FF; the
                // test `the_second_register_bel_of_a_half_is_always_refused` ties this to it).
                continue;
            }
            // `verdict` takes its trials as one array; only the rare paths that need it build it.
            let full = |lab: &mut ResidentLab| {
                let mut in_view: [Option<&BelPatchV2>; MAX_TRIALS] = [None; MAX_TRIALS];
                in_view[..trials.len()].copy_from_slice(trials);
                in_view[trials.len()] = Some(&at);
                Self::verdict(
                    lab,
                    limit,
                    &in_view[..trials.len() + 1],
                    query,
                    at.alm,
                    true,
                )
                .status
                    == LAB_LEGAL
            };
            let legal = match Self::bel_passes_alm_and_total(lab, limit, trials, &at) {
                false => false,
                true if is_lut => !is_mlab || full(lab),
                true => match Self::control_legal(lab, trials, &at) {
                    Some(legal) => legal && (!is_mlab || full(lab)),
                    // More control nets than the mirror holds: `verdict` decides.
                    None => full(lab),
                },
            };
            if legal {
                return Ok(Some(index));
            }
        }
        Ok(None)
    }

    /// For the scan, which needs the answer and not the reason: whether the candidate's ALM passes
    /// the ALM rule and the LAB stays within its input limit, with the trials and the candidate
    /// (at the bel its `alm` and `slot` name) in view. These are `verdict`'s first two predicates
    /// asked in order of cost: on a crowded LAB three bels in four fail the ALM rule alone, so the
    /// input counts are recomputed only for a bel that has passed it, and the rule's rejection
    /// record is discarded.
    fn bel_passes_alm_and_total(
        lab: &ResidentLab,
        limit: i32,
        trials: &[Option<&BelPatchV2>],
        candidate: &BelPatchV2,
    ) -> bool {
        let facts = &lab.facts;
        let index = candidate.alm as usize;
        let with = Some(candidate);
        if !check_alm(
            &Self::view_with(facts, trials, with, index),
            index,
            &mut Discard,
        ) {
            return false;
        }
        let mut touched = 0u32;
        let mut total = lab.total_inputs;
        for patch in trials.iter().flatten().copied().chain(with) {
            let alm = patch.alm as usize;
            if touched & (1 << alm) == 0 {
                touched |= 1 << alm;
                total += recompute_inputs(&Self::view_with(facts, trials, with, alm))
                    - lab.alm_inputs[alm];
            }
        }
        total <= limit
    }

    /// `verdict`'s third predicate: whether the LAB's control sets are legal with the trials'
    /// register rows in view. `None` when the mirror cannot hold the trials' control nets, where
    /// `verdict` takes the capture path's projection. The answer is of the bels the registers
    /// sit in, not of the LAB: the rules' second walk gives data lines by first fit over lists
    /// that overlap between kinds, so a net that serves two kinds may fit at one bel and not at
    /// another (design 16.4, and the hostile scan oracle below).
    fn control_legal(
        lab: &mut ResidentLab,
        trials: &[Option<&BelPatchV2>],
        candidate: &BelPatchV2,
    ) -> Option<bool> {
        let ResidentLab { facts, mirror, .. } = lab;
        let mut rows: [(usize, &LabFfV2); MAX_TRIALS] = [(0, &facts.alm[0].ff[0]); MAX_TRIALS];
        let mut row_count = 0;
        for patch in trials.iter().flatten().copied().chain(Some(candidate)) {
            if patch.slot as usize >= LUTS {
                rows[row_count] = (
                    patch.alm as usize * FFS + (patch.slot as usize - LUTS),
                    &patch.ff,
                );
                row_count += 1;
            }
        }
        let undo = mirror.apply_trials(&rows[..row_count])?;
        let control = control_verdict(&mirror.snapshot);
        mirror.undo_trials(&undo);
        Some(control.status == LEGAL)
    }

    /// The second register bel of an ALM half (slots 3 and 5 of the six): `check_alm` refuses any
    /// register there.
    fn second_register_bel(slot: usize) -> bool {
        slot >= LUTS && (slot - LUTS) % 2 == 1
    }

    /// The patches' shapes, one per bel, and no more trials than `room`.
    fn validate(patches: &[BelPatchV2], room: usize) -> Result<(), ResidentError> {
        let mut seen = 0u64;
        for patch in patches {
            let bit = 1u64 << (patch.alm as usize * (LUTS + FFS) + patch.slot as usize);
            if !Self::patch_valid(patch) || seen & bit != 0 {
                return Err(ResidentError::Patch);
            }
            seen |= bit;
        }
        if patches.iter().filter(|p| p.commit == 0).count() > room {
            return Err(ResidentError::Patch);
        }
        Ok(())
    }

    /// Applies the commits of validated patches and returns the trials that change something.
    #[allow(clippy::type_complexity)]
    fn sync<'p>(
        lab: &mut ResidentLab,
        patches: &'p [BelPatchV2],
        recompute: bool,
    ) -> Result<([Option<&'p BelPatchV2>; MAX_TRIALS], usize), ResidentError> {
        // A caller's count describes the live ALM, trials included; an ALM that
        // takes a commit and a trial in the same call gets its committed count
        // recomputed from the facts instead.
        let mut trial_mask = 0u32;
        for patch in patches {
            if patch.commit == 0 && Self::bel_changes(&lab.facts.alm[patch.alm as usize], patch) {
                trial_mask |= 1 << patch.alm;
            }
        }
        // Commits first, so a failed mirror update (table full) leaves the
        // facts consistent: the mirror is updated before the facts.
        let mut trials: [Option<&BelPatchV2>; MAX_TRIALS] = [None; MAX_TRIALS];
        let mut trial_count = 0;
        for patch in patches {
            let index = patch.alm as usize;
            if !Self::bel_changes(&lab.facts.alm[index], patch) {
                continue;
            }
            if patch.commit == 0 {
                trials[trial_count] = Some(patch);
                trial_count += 1;
                continue;
            }
            if patch.slot as usize >= LUTS {
                let row = index * FFS + (patch.slot as usize - LUTS);
                if lab.mirror.set(row, &patch.ff).is_none() {
                    return Err(ResidentError::Patch);
                }
            }
            Self::apply_bel(&mut lab.facts.alm[index], patch);
            let (bit, taken) = Self::bel_bit(patch);
            lab.occupied = if taken {
                lab.occupied | bit
            } else {
                lab.occupied & !bit
            };
            let inputs = if recompute || trial_mask & (1 << index) != 0 {
                recompute_inputs(&AlmView::of(&lab.facts.alm[index]))
            } else {
                patch.alm_inputs
            };
            lab.total_inputs += inputs - lab.alm_inputs[index];
            lab.alm_inputs[index] = inputs;
        }
        Ok((trials, trial_count))
    }

    /// An ALM's slots with the trials that touch it substituted, by reference.
    /// The committed bel a patch names, as its occupancy bit, and whether the patch occupies it.
    fn bel_bit(patch: &BelPatchV2) -> (u64, bool) {
        let bit = 1u64 << (patch.alm as usize * (LUTS + FFS) + patch.slot as usize);
        let taken = if (patch.slot as usize) < LUTS {
            patch.lut.occupied != 0
        } else {
            patch.ff.occupied != 0
        };
        (bit, taken)
    }

    fn view<'a>(
        facts: &'a LabFactsV2,
        trials: &[Option<&'a BelPatchV2>],
        index: usize,
    ) -> AlmView<'a> {
        Self::view_with(facts, trials, None, index)
    }

    /// An ALM as the trials leave it, with one more patch in view beside them: the scan's
    /// candidate, which moves from bel to bel while the trials stay.
    fn view_with<'a>(
        facts: &'a LabFactsV2,
        trials: &[Option<&'a BelPatchV2>],
        extra: Option<&'a BelPatchV2>,
        index: usize,
    ) -> AlmView<'a> {
        let mut view = AlmView::of(&facts.alm[index]);
        for patch in trials.iter().flatten().copied().chain(extra) {
            if patch.alm as usize != index {
                continue;
            }
            if (patch.slot as usize) < LUTS {
                view.lut[patch.slot as usize] = &patch.lut;
            } else {
                view.ff[patch.slot as usize - LUTS] = &patch.ff;
            }
        }
        view
    }

    /// An ALM with the trials that touch it applied, composed on demand.
    fn compose(facts: &LabFactsV2, trials: &[Option<&BelPatchV2>], index: usize) -> AlmFactsV2 {
        let mut alm = facts.alm[index];
        for patch in trials.iter().flatten() {
            if patch.alm as usize == index {
                Self::apply_bel(&mut alm, patch);
            }
        }
        alm
    }

    fn verdict(
        lab: &mut ResidentLab,
        limit: i32,
        trials: &[Option<&BelPatchV2>],
        query: u32,
        query_alm: u32,
        recompute: bool,
    ) -> LabVerdictV2 {
        let ResidentLab {
            facts,
            alm_inputs,
            total_inputs,
            mirror,
            ..
        } = lab;
        let facts: &LabFactsV2 = facts;
        let mut touched = 0u32;
        let mut counts = *alm_inputs;
        let mut total = *total_inputs;
        for patch in trials.iter().flatten() {
            let index = patch.alm as usize;
            let inputs = if recompute {
                recompute_inputs(&Self::view(facts, trials, index))
            } else {
                patch.alm_inputs
            };
            if touched & (1 << index) == 0 {
                touched |= 1 << index;
                total += inputs - counts[index];
                counts[index] = inputs;
            }
        }
        let mut result = LabAssessmentV2::empty(facts);
        result.recomputed_input_count = counts;
        result.recomputed_valid_mask = (1 << ALMS) - 1;
        let done = |result: &LabAssessmentV2| LabVerdictV2::from(result);
        let check = |index: usize, result: &mut LabAssessmentV2| -> bool {
            check_alm(&Self::view(facts, trials, index), index, result)
        };
        if query == QUERY_WHOLE_LAB {
            for alm in 0..ALMS {
                if !check(alm, &mut result) {
                    return done(&result);
                }
            }
        } else if !check(query_alm as usize, &mut result) {
            return done(&result);
        }
        if total > limit {
            reject(
                &mut result,
                LAB_INPUT_LIMIT,
                u32::MAX,
                u32::MAX,
                total,
                limit,
            );
            return done(&result);
        }
        if query != QUERY_COMB_BEL {
            // The control rules over the mirror, with the trials' register rows substituted.
            let mut rows: [(usize, &LabFfV2); MAX_TRIALS] = [(0, &facts.alm[0].ff[0]); MAX_TRIALS];
            let mut row_count = 0;
            for patch in trials.iter().flatten() {
                if patch.slot as usize >= LUTS {
                    rows[row_count] = (
                        patch.alm as usize * FFS + (patch.slot as usize - LUTS),
                        &patch.ff,
                    );
                    row_count += 1;
                }
            }
            let in_place = match mirror.apply_trials(&rows[..row_count]) {
                Some(undo) => {
                    let control = control_verdict(&mirror.snapshot);
                    mirror.undo_trials(&undo);
                    Some(control)
                }
                None => None,
            };
            let control = match in_place {
                Some(control) => control,
                None => {
                    // More distinct control nets than the model holds: the capture
                    // path's projection decides, at its cost.
                    let mut full = *facts;
                    for index in 0..ALMS {
                        if touched & (1 << index) != 0 {
                            full.alm[index] = Self::compose(facts, trials, index);
                        }
                    }
                    full.query = query;
                    full.query_alm = query_alm;
                    let a = evaluate_facts(&full);
                    ControlVerdict {
                        status: a.control.status,
                        reason: a.control.reason,
                        kind: a.control.control_kind,
                        ff_slot: a.control.ff_slot,
                        mask: a.control.resource_mask,
                    }
                }
            };
            result.control_valid = 1;
            result.control.status = control.status;
            result.control.reason = control.reason;
            result.control.control_kind = control.kind;
            result.control.ff_slot = control.ff_slot;
            result.control.resource_mask = control.mask;
            if control.status != LEGAL {
                let alm = control.ff_slot / 4;
                let slot = control.ff_slot % 4;
                reject(
                    &mut result,
                    CONTROL_CONFLICT,
                    alm,
                    slot,
                    control.reason as i32,
                    0,
                );
                return done(&result);
            }
        }
        if facts.is_mlab != 0 {
            // MLABs are few: compose every touched ALM for the group check.
            let mut full = *facts;
            for index in 0..ALMS {
                if touched & (1 << index) != 0 {
                    full.alm[index] = Self::compose(facts, trials, index);
                }
            }
            check_mlab(true, |i| &full.alm[i], &mut result);
        }
        done(&result)
    }

    /// The capture path's verdict over the committed facts with the trials in
    /// view: the oracle the incremental evaluation must equal.
    pub fn reference(
        &self,
        lab: usize,
        trials: &[BelPatchV2],
        query: u32,
        query_alm: u32,
    ) -> Result<LabVerdictV2, ResidentError> {
        let mut facts = *self.facts(lab)?;
        if !query_valid(query, query_alm) {
            return Err(ResidentError::Query);
        }
        for patch in trials {
            Self::apply_bel(&mut facts.alm[patch.alm as usize], patch);
        }
        facts.query = query;
        facts.query_alm = query_alm;
        Ok(LabVerdictV2::from(&evaluate_facts(&facts)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::ControlSignalV1;

    struct Lcg(u64);
    impl Lcg {
        fn next(&mut self, bound: u32) -> u32 {
            self.0 = self
                .0
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            ((self.0 >> 33) as u32) % bound
        }
    }

    fn random_signal(rng: &mut Lcg, keys: &[u32]) -> ControlSignalV1 {
        if rng.next(3) == 0 {
            return ControlSignalV1::default();
        }
        let net_id = keys[rng.next(keys.len() as u32) as usize];
        let mut flags = 0;
        if rng.next(4) == 0 {
            flags |= INVERTED;
        }
        // A net is global or not: the flag follows the key, as it follows the net in the arch.
        if net_id % 5 == 0 {
            flags |= GLOBAL;
        }
        ControlSignalV1 { net_id, flags }
    }

    fn random_lut(rng: &mut Lcg, keys: &[u32]) -> LabLutV2 {
        let mut lut = LabLutV2::default();
        if rng.next(3) == 0 {
            return lut;
        }
        lut.occupied = 1;
        lut.input_count = 1 + rng.next(6);
        let mut used = 0;
        for pin in 0..lut.input_count as usize {
            if rng.next(5) != 0 {
                lut.input_net[pin] = keys[rng.next(keys.len() as u32) as usize];
                used += 1;
            }
        }
        lut.used_input_count = used;
        lut.bits_count = 1 << (lut.input_count.min(6));
        lut.mlab_group = -1;
        lut.comb_out_net = keys[rng.next(keys.len() as u32) as usize];
        lut.is_carry = u32::from(rng.next(6) == 0);
        lut.wclk = random_signal(rng, keys);
        lut.we = random_signal(rng, keys);
        lut
    }

    fn random_ff(rng: &mut Lcg, keys: &[u32], slot: usize) -> LabFfV2 {
        // Mostly one register per half with the LAB's clock and nothing else,
        // so legal control sets are common; sometimes a random control set.
        let mut ff = LabFfV2::default();
        if slot % 2 == 1 || rng.next(3) == 0 {
            return ff;
        }
        ff.occupied = 1;
        if rng.next(3) != 0 {
            ff.datain_net = keys[rng.next(keys.len() as u32) as usize];
        }
        if rng.next(8) == 0 {
            ff.sdata_net = keys[rng.next(keys.len() as u32) as usize];
        }
        ff.control[0] = ControlSignalV1 {
            net_id: keys[0],
            flags: 0,
        };
        if rng.next(4) == 0 {
            for control in &mut ff.control {
                *control = random_signal(rng, &keys[..3]);
            }
        }
        ff
    }

    fn random_patch(rng: &mut Lcg, keys: &[u32], alm: u32, slot: u32) -> BelPatchV2 {
        let mut patch = BelPatchV2 {
            alm,
            slot,
            commit: u32::from(rng.next(3) != 0),
            ..BelPatchV2::default()
        };
        if (slot as usize) < LUTS {
            patch.lut = random_lut(rng, keys);
        } else {
            patch.ff = random_ff(rng, keys, slot as usize - LUTS);
        }
        patch
    }

    fn random_query(rng: &mut Lcg) -> (u32, u32) {
        match rng.next(3) {
            0 => (QUERY_COMB_BEL, rng.next(ALMS as u32)),
            1 => (QUERY_FF_BEL, rng.next(ALMS as u32)),
            _ => (QUERY_WHOLE_LAB, u32::MAX),
        }
    }

    /// Random commits and trials against the capture path over a shadow of the
    /// committed facts, with the control mirror exercised through many register
    /// changes, trial rows, and legal and illegal control sets.
    #[test]
    fn incremental_verdicts_equal_the_capture_path_over_random_patch_sequences() {
        let keys: Vec<u32> = (0..12).map(|i| 7_919 * (i + 3) + 1).collect();
        let mut rng = Lcg(20_260_919);
        let mut resident = ResidentLabs::new(3, 42).unwrap();
        let mut shadow: Vec<LabFactsV2> = (0..3)
            .map(|lab| LabFactsV2 {
                input_limit: 42,
                is_mlab: u32::from(lab == 2),
                ..LabFactsV2::default()
            })
            .collect();
        for lab in 0..3 {
            resident.reset(lab, lab == 2).unwrap();
        }
        let (mut legal, mut trials, mut ff_legal) = (0, 0, 0);
        let mut bursts = 0;
        let mut unchanged = 0;
        let (mut scans, mut scans_later, mut scans_none) = (0u32, 0u32, 0u32);
        for step in 0..8000u64 {
            let lab = rng.next(3) as usize;
            let count = rng.next(4) as usize;
            let mut patches: Vec<BelPatchV2> = Vec::new();
            if step % 40 == 39 {
                // Shape 6: a burst of commits over every bel, as after a reset.
                for alm in 0..ALMS as u32 {
                    for slot in 0..(LUTS + FFS) as u32 {
                        let mut patch = random_patch(&mut rng, &keys, alm, slot);
                        patch.commit = 1;
                        patches.push(patch);
                    }
                }
                bursts += 1;
            }
            while patches.len() < count {
                let alm = rng.next(ALMS as u32);
                let slot = rng.next((LUTS + FFS) as u32);
                if patches.iter().any(|p| p.alm == alm && p.slot == slot) {
                    continue;
                }
                if step % 10 == 9 {
                    // Shape 7: the held facts resent, as a trial, which changes nothing.
                    let held = &shadow[lab].alm[alm as usize];
                    let mut patch = BelPatchV2 {
                        alm,
                        slot,
                        commit: 0,
                        ..BelPatchV2::default()
                    };
                    if (slot as usize) < LUTS {
                        patch.lut = held.lut[slot as usize];
                    } else {
                        patch.ff = held.ff[slot as usize - LUTS];
                    }
                    patches.push(patch);
                    unchanged += 1;
                    continue;
                }
                patches.push(random_patch(&mut rng, &keys, alm, slot));
            }
            let (query, query_alm) = random_query(&mut rng);
            // The caller's count for each patched ALM: the ALM as it stands with every
            // patch of this call in place, as the arch keeps it at query time.
            let mut with_all = shadow[lab];
            for patch in &patches {
                ResidentLabs::apply_bel(&mut with_all.alm[patch.alm as usize], patch);
            }
            for patch in &mut patches {
                patch.alm_inputs =
                    recompute_inputs(&AlmView::of(&with_all.alm[patch.alm as usize]));
            }
            let recompute = step % 2 == 0;
            let result = resident
                .evaluate(lab, &patches, query, query_alm, recompute)
                .unwrap();
            let mut expected = shadow[lab];
            let mut in_view: Vec<BelPatchV2> = Vec::new();
            for patch in &patches {
                ResidentLabs::apply_bel(&mut expected.alm[patch.alm as usize], patch);
                if patch.commit == 1 {
                    ResidentLabs::apply_bel(&mut shadow[lab].alm[patch.alm as usize], patch);
                } else {
                    in_view.push(*patch);
                    trials += 1;
                }
            }
            expected.query = query;
            expected.query_alm = query_alm;
            let reference = LabVerdictV2::from(&evaluate_facts(&expected));
            assert_eq!(result, reference, "step {step}");
            assert_eq!(
                resident.facts(lab).unwrap().alm,
                shadow[lab].alm,
                "step {step}"
            );
            let mut from_facts = 0u64;
            for (index, alm) in shadow[lab].alm.iter().enumerate() {
                for slot in 0..(LUTS + FFS) {
                    let taken = if slot < LUTS {
                        alm.lut[slot].occupied
                    } else {
                        alm.ff[slot - LUTS].occupied
                    };
                    from_facts |= u64::from(taken != 0) << (index * (LUTS + FFS) + slot);
                }
            }
            assert_eq!(
                resident.labs[lab].occupied, from_facts,
                "occupancy at step {step}"
            );
            assert_eq!(
                resident.reference(lab, &in_view, query, query_alm).unwrap(),
                reference
            );
            legal += u32::from(result.status == LAB_LEGAL);
            ff_legal += u32::from(query != QUERY_COMB_BEL && result.status == LAB_LEGAL);

            // Shape 8: a scan over free bels of this state, the step's trials resent to stay in
            // view, against the capture path answering bel by bel. Nothing of it is applied.
            if step % 3 == 0 && in_view.len() < MAX_TRIALS {
                let want_lut = rng.next(2) == 0;
                let mut candidate = BelPatchV2::default();
                loop {
                    if want_lut {
                        candidate.lut = random_lut(&mut rng, &keys);
                        if candidate.lut.occupied == 1 {
                            break;
                        }
                    } else {
                        let ff_slot = rng.next(FFS as u32) as usize;
                        candidate.ff = random_ff(&mut rng, &keys, ff_slot);
                        if candidate.ff.occupied == 1 {
                            break;
                        }
                    }
                }
                let mut order: Vec<u8> = Vec::new();
                for alm in 0..ALMS {
                    for slot in 0..(LUTS + FFS) {
                        let free = if slot < LUTS {
                            expected.alm[alm].lut[slot].occupied == 0
                        } else {
                            expected.alm[alm].ff[slot - LUTS].occupied == 0
                        };
                        if free && (slot < LUTS) == want_lut && rng.next(4) != 0 {
                            order.push((alm * (LUTS + FFS) + slot) as u8);
                        }
                    }
                }
                for i in (1..order.len()).rev() {
                    order.swap(i, rng.next(i as u32 + 1) as usize);
                }
                let found = resident
                    .evaluate_scan(lab, &in_view, &candidate, &order, step % 2 == 0)
                    .unwrap();
                let mut reference_found = None;
                for (index, &bel) in order.iter().enumerate() {
                    let mut at = candidate;
                    at.alm = u32::from(bel) / (LUTS + FFS) as u32;
                    at.slot = u32::from(bel) % (LUTS + FFS) as u32;
                    let mut with = expected;
                    ResidentLabs::apply_bel(&mut with.alm[at.alm as usize], &at);
                    with.query = if want_lut {
                        QUERY_COMB_BEL
                    } else {
                        QUERY_FF_BEL
                    };
                    with.query_alm = at.alm;
                    if evaluate_facts(&with).status == LAB_LEGAL {
                        reference_found = Some(index);
                        break;
                    }
                }
                assert_eq!(found, reference_found, "scan at step {step}: {order:?}");
                assert_eq!(
                    resident.facts(lab).unwrap().alm,
                    shadow[lab].alm,
                    "scan at step {step}"
                );
                scans += 1;
                scans_later += u32::from(matches!(found, Some(index) if index > 0));
                scans_none += u32::from(found.is_none() && !order.is_empty());
            }
        }
        assert!(
            scans >= 2000 && scans_later >= 100 && scans_none >= 100,
            "{scans} scans, {scans_later} found past the first bel, {scans_none} found nothing"
        );
        assert!(trials > 500, "{trials}");
        assert!(
            legal > 100,
            "the generator should produce legal LABs too: {legal}"
        );
        assert!(
            ff_legal > 30,
            "legal control verdicts should occur: {ff_legal}"
        );
        assert!(
            bursts >= 190 && unchanged >= 300,
            "{bursts} bursts, {unchanged} unchanged"
        );
    }

    /// The scan skips the second register bel of a half without asking the ALM rule; this holds
    /// the shortcut to the rule over random ALMs, registers, and both such bels of each ALM.
    #[test]
    fn the_second_register_bel_of_a_half_is_always_refused() {
        let keys: Vec<u32> = (0..12).map(|i| 7_919 * (i + 3) + 1).collect();
        let mut rng = Lcg(55_511);
        let mut checked = 0;
        for _ in 0..4000 {
            let mut alm = AlmFactsV2::default();
            for slot in 0..LUTS {
                alm.lut[slot] = random_lut(&mut rng, &keys);
            }
            for slot in 0..FFS {
                // The other three register bels in any state, legal or not.
                alm.ff[slot] = random_ff(&mut rng, &keys, slot);
            }
            for slot in 0..(LUTS + FFS) {
                assert_eq!(
                    ResidentLabs::second_register_bel(slot),
                    slot == LUTS + 1 || slot == LUTS + 3
                );
            }
            for odd in [1usize, 3] {
                let mut with = alm;
                // The generator leaves odd bels empty; an even bel's register goes there.
                loop {
                    with.ff[odd] = random_ff(&mut rng, &keys, 0);
                    if with.ff[odd].occupied == 1 {
                        break;
                    }
                }
                let mut result = LabAssessmentV2::default();
                assert!(!check_alm(&AlmView::of(&with), 0, &mut result));
                checked += 1;
            }
        }
        assert_eq!(checked, 8000);
    }

    /// The main oracle's LABs almost never make a control refusal depend on the bel, and a scan
    /// that wrongly ended at such a refusal passed it (the first form of design 16.4 did, and the
    /// full core's checksum caught it; a mutation check confirmed this oracle fails on it). This
    /// oracle builds the LABs where it matters: one small palette of nets for every control kind,
    /// clocks that are not global, registers with no data path of their own so the control rules
    /// decide. Every scan is held to the capture path bel by bel, and the run must contain scans
    /// whose first legal bel comes after a bel the control rules refused.
    #[test]
    fn scans_over_hostile_control_labs_equal_the_capture_path() {
        let keys: Vec<u32> = (0..12).map(|i| 7_919 * (i + 3) + 1).collect();
        let mut rng = Lcg(16_041);
        let (mut scans, mut legal_after_control_refusal, mut none_found) = (0u32, 0u32, 0u32);
        for _ in 0..12_000 {
            let mut resident = ResidentLabs::new(1, 42).unwrap();
            resident.reset(0, false).unwrap();
            let shared = 2 + rng.next(3);
            let signal = |rng: &mut Lcg, absent: u32| {
                if rng.next(absent) != 0 {
                    return ControlSignalV1::default();
                }
                let net_id = keys[rng.next(shared) as usize];
                ControlSignalV1 {
                    net_id,
                    flags: if net_id % 5 == 0 { GLOBAL } else { 0 },
                }
            };
            let clock_net = keys[if rng.next(2) == 0 { 3 } else { 0 }];
            let clock = ControlSignalV1 {
                net_id: clock_net,
                flags: if clock_net % 5 == 0 { GLOBAL } else { 0 },
            };
            let mut patches = Vec::new();
            for alm in 0..ALMS as u32 {
                for slot in [LUTS as u32, LUTS as u32 + 2] {
                    if rng.next(4) != 0 {
                        continue;
                    }
                    let mut patch = BelPatchV2 {
                        alm,
                        slot,
                        commit: 1,
                        ..BelPatchV2::default()
                    };
                    patch.ff.occupied = 1;
                    patch.ff.control[0] = clock;
                    for kind in 1..patch.ff.control.len() {
                        patch.ff.control[kind] = signal(&mut rng, 3);
                    }
                    patches.push(patch);
                }
            }
            if resident
                .evaluate(0, &patches, QUERY_WHOLE_LAB, u32::MAX, true)
                .is_err()
            {
                continue;
            }
            let mut candidate = BelPatchV2::default();
            candidate.ff.occupied = 1;
            candidate.ff.control[0] = clock;
            for kind in 1..candidate.ff.control.len() {
                candidate.ff.control[kind] = signal(&mut rng, 2);
            }
            let facts = *resident.facts(0).unwrap();
            let mut order: Vec<u8> = Vec::new();
            for alm in 0..ALMS {
                for ff in 0..FFS {
                    if facts.alm[alm].ff[ff].occupied == 0 && rng.next(5) != 0 {
                        order.push((alm * (LUTS + FFS) + LUTS + ff) as u8);
                    }
                }
            }
            for i in (1..order.len()).rev() {
                order.swap(i, rng.next(i as u32 + 1) as usize);
            }
            let found = resident
                .evaluate_scan(0, &[], &candidate, &order, rng.next(2) == 0)
                .unwrap();
            let mut expected = None;
            let mut control_refused = false;
            for (index, &bel) in order.iter().enumerate() {
                let mut at = candidate;
                at.alm = u32::from(bel) / (LUTS + FFS) as u32;
                at.slot = u32::from(bel) % (LUTS + FFS) as u32;
                let mut with = facts;
                ResidentLabs::apply_bel(&mut with.alm[at.alm as usize], &at);
                with.query = QUERY_FF_BEL;
                with.query_alm = at.alm;
                let reference = evaluate_facts(&with);
                if reference.status == LAB_LEGAL {
                    expected = Some(index);
                    break;
                }
                control_refused |= reference.reason == CONTROL_CONFLICT;
            }
            assert_eq!(found, expected, "{order:?}");
            assert_eq!(resident.facts(0).unwrap().alm, facts.alm);
            scans += 1;
            legal_after_control_refusal += u32::from(expected.is_some() && control_refused);
            none_found += u32::from(expected.is_none() && !order.is_empty());
        }
        assert!(
            scans > 5_000 && none_found > 200,
            "{scans} scans, {none_found} found nothing"
        );
        assert!(
            legal_after_control_refusal > 20,
            "only {legal_after_control_refusal} scans found a legal bel after a control refusal"
        );
    }

    #[test]
    fn invalid_patches_apply_nothing_and_scopes_are_checked() {
        let mut resident = ResidentLabs::new(1, 42).unwrap();
        assert_eq!(
            resident.evaluate(0, &[], QUERY_WHOLE_LAB, u32::MAX, true),
            Err(ResidentError::Uninitialised)
        );
        resident.reset(0, false).unwrap();
        assert_eq!(resident.reset(1, false), Err(ResidentError::Lab));
        let mut good = BelPatchV2 {
            commit: 1,
            ..BelPatchV2::default()
        };
        good.lut.occupied = 1;
        good.lut.input_count = 2;
        good.lut.used_input_count = 2;
        good.lut.bits_count = 4;
        good.lut.mlab_group = -1;
        good.lut.input_net[..2].copy_from_slice(&[500, 9_000_000]);
        good.lut.comb_out_net = 77;
        let mut bad = good;
        bad.alm = 1;
        bad.lut.used_input_count = 3; // more used than wired
        assert_eq!(
            resident.evaluate(0, &[good, bad], QUERY_COMB_BEL, 0, true),
            Err(ResidentError::Patch)
        );
        let twice = good;
        assert_eq!(
            resident.evaluate(0, &[good, twice], QUERY_COMB_BEL, 0, true),
            Err(ResidentError::Patch)
        );
        let mut many: Vec<BelPatchV2> = Vec::new();
        for slot in 0..(MAX_TRIALS as u32 + 1) {
            let mut trial = good;
            trial.alm = slot / 6;
            trial.slot = slot % 6;
            trial.commit = 0;
            if trial.slot >= LUTS as u32 {
                trial.lut = LabLutV2::default();
                trial.ff.occupied = 1;
            }
            many.push(trial);
        }
        assert_eq!(
            resident.evaluate(0, &many, QUERY_COMB_BEL, 0, true),
            Err(ResidentError::Patch)
        );
        let mut wrong_half = good;
        wrong_half.slot = 3; // a register slot carrying LUT facts
        assert_eq!(
            resident.evaluate(0, &[wrong_half], QUERY_COMB_BEL, 0, true),
            Err(ResidentError::Patch)
        );
        assert_eq!(resident.facts(0).unwrap().alm[0], AlmFactsV2::default());
        assert_eq!(
            resident.evaluate(0, &[good], QUERY_WHOLE_LAB, 3, true),
            Err(ResidentError::Query)
        );
        let result = resident
            .evaluate(0, &[good], QUERY_COMB_BEL, 0, true)
            .unwrap();
        assert_eq!(result.status, LAB_LEGAL);
        assert_eq!(result.recomputed_input_count[0], 2);
        // A trial is in view for its evaluation only.
        let mut odd = BelPatchV2 {
            slot: 3,
            ..BelPatchV2::default()
        };
        odd.ff.occupied = 1;
        let mut first = odd;
        first.slot = 2;
        first.commit = 1;
        assert_eq!(
            resident
                .evaluate(0, &[first], QUERY_FF_BEL, 0, true)
                .unwrap()
                .status,
            LAB_LEGAL
        );
        let result = resident.evaluate(0, &[odd], QUERY_FF_BEL, 0, true).unwrap();
        assert_eq!((result.status, result.reason), (LAB_ILLEGAL, ODD_FF));
        assert_eq!((result.failing_alm, result.failing_slot), (0, 1));
        assert_eq!(resident.facts(0).unwrap().alm[0].ff[1], LabFfV2::default());
        let result = resident.evaluate(0, &[], QUERY_FF_BEL, 0, true).unwrap();
        assert_eq!(result.status, LAB_LEGAL);
    }

    #[test]
    fn the_control_mirror_reports_conflicts_like_the_capture_path() {
        let mut resident = ResidentLabs::new(1, 42).unwrap();
        resident.reset(0, false).unwrap();
        let clock = |net_id: u32| ControlSignalV1 { net_id, flags: 0 };
        let mut a = BelPatchV2 {
            alm: 0,
            slot: 2,
            commit: 1,
            ..BelPatchV2::default()
        };
        a.ff.occupied = 1;
        a.ff.control[0] = clock(11);
        assert_eq!(
            resident
                .evaluate(0, &[a], QUERY_FF_BEL, 0, true)
                .unwrap()
                .status,
            LAB_LEGAL
        );
        // A second clock as a trial: illegal, with the failing register named, and not persisted.
        let mut b = a;
        b.alm = 1;
        b.commit = 0;
        b.ff.control[0] = clock(12);
        let result = resident.evaluate(0, &[b], QUERY_FF_BEL, 1, true).unwrap();
        assert_eq!(
            (result.status, result.reason),
            (LAB_ILLEGAL, CONTROL_CONFLICT)
        );
        assert_eq!((result.failing_alm, result.failing_slot), (1, 0));
        assert_eq!(
            result,
            resident.reference(0, &[b], QUERY_FF_BEL, 1).unwrap()
        );
        assert_eq!(resident.facts(0).unwrap().alm[1].ff[0], LabFfV2::default());
        assert_eq!(
            resident
                .evaluate(0, &[], QUERY_FF_BEL, 1, true)
                .unwrap()
                .status,
            LAB_LEGAL
        );
        // Releasing the only user of a net frees its id for a new net.
        let mut gone = a;
        gone.ff = LabFfV2::default();
        assert_eq!(
            resident
                .evaluate(0, &[gone], QUERY_FF_BEL, 0, true)
                .unwrap()
                .status,
            LAB_LEGAL
        );
        assert_eq!(
            resident.labs[0]
                .mirror
                .refs
                .iter()
                .filter(|&&r| r != 0)
                .count(),
            0
        );
        b.commit = 1;
        assert_eq!(
            resident
                .evaluate(0, &[b], QUERY_FF_BEL, 1, true)
                .unwrap()
                .status,
            LAB_LEGAL
        );
        assert_eq!(
            resident.labs[0]
                .mirror
                .refs
                .iter()
                .filter(|&&r| r != 0)
                .count(),
            1
        );
    }
}
