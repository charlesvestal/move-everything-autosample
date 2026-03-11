import {
    MidiCC,
    MoveShift, MoveMainKnob, MoveMainButton, MoveBack,
    MoveCapture, MoveRec,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    MoveLeft, MoveRight,
    White, Black, DarkGrey, LightGrey
} from '/data/UserData/move-anything/shared/constants.mjs';

import { decodeDelta } from '/data/UserData/move-anything/shared/input_filter.mjs';
import { announce } from '/data/UserData/move-anything/shared/screen_reader.mjs';
import { log as uniLog } from '/data/UserData/move-anything/shared/logger.mjs';

function debugLog(msg) { uniLog("SampleRobot", msg); }

/* ── Views ── */
var VIEW_SETUP      = 0;
var VIEW_NAMING     = 1;
var VIEW_SAMPLING   = 2;
var VIEW_PROCESSING = 3;
var VIEW_DONE       = 4;

var currentView = VIEW_SETUP;
var shiftHeld = false;

/* ── Setup params ── */
var NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];

function noteNameFmt(n) {
    return NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 2);
}

function onOffFmt(v) { return v ? 'ON' : 'OFF'; }

var params = [
    { key: 'range_low',       label: 'Low Note',  val: 36,  min: 0,   max: 127, step: 1,   fmt: noteNameFmt },
    { key: 'range_high',      label: 'Hi Note',   val: 84,  min: 0,   max: 127, step: 1,   fmt: noteNameFmt },
    { key: 'key_zones',       label: 'Zones',     val: 8,   min: 1,   max: 24,  step: 1,   fmt: null },
    { key: 'velocity_layers', label: 'Layers',    val: 3,   min: 1,   max: 8,   step: 1,   fmt: null },
    { key: 'hold_duration',   label: 'Hold (s)',  val: 3.0, min: 0.5, max: 30,  step: 0.5, fmt: function(v) { return v.toFixed(1); } },
    { key: 'loop_detect',     label: 'Loop',      val: 1,   min: 0,   max: 1,   step: 1,   fmt: onOffFmt },
    { key: 'midi_channel',    label: 'MIDI Ch',   val: 1,   min: 1,   max: 16,  step: 1,   fmt: null },
];
var selectedParam = 0;

/* ── Naming state ── */
var instrumentName = '';
var charIndex = 0;
var CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_';

/* ── Back-hold tracking for cancel ── */
var backHeldSince = 0;

/* ── Helper: clamp ── */
function clamp(v, lo, hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ── Helper: format param value ── */
function fmtVal(p) {
    if (p.fmt) return p.fmt(p.val);
    return String(p.val);
}

/* ── Helper: total sample count ── */
function totalSamples() {
    var zones = params[2].val;   /* key_zones */
    var layers = params[3].val;  /* velocity_layers */
    return zones * layers;
}

/* ══════════════════════════════════════════════
   DRAW FUNCTIONS
   ══════════════════════════════════════════════ */

function drawSetup() {
    print(4, 0, "SAMPLE ROBOT", White);

    /* Visible rows for params: y 10..54, 8px each = ~5 rows */
    var visibleRows = 5;
    var scrollTop = 0;
    if (selectedParam >= visibleRows) {
        scrollTop = selectedParam - visibleRows + 1;
    }

    for (var i = 0; i < visibleRows && (i + scrollTop) < params.length; i++) {
        var pi = i + scrollTop;
        var p = params[pi];
        var y = 10 + i * 9;
        if (pi === selectedParam) {
            fill_rect(0, y, 128, 9, White);
            print(2, y + 1, p.label, Black);
            print(76, y + 1, fmtVal(p), Black);
        } else {
            print(2, y + 1, p.label, White);
            print(76, y + 1, fmtVal(p), LightGrey);
        }
    }

    print(2, 56, "Total:" + totalSamples(), LightGrey);
    print(72, 56, "[Rec]Start", LightGrey);
}

function drawNaming() {
    print(4, 0, "NAME INSTRUMENT", White);

    /* Current name with cursor */
    var display = instrumentName + '_';
    print(4, 14, display, White);

    /* Current char selection */
    var ch = CHARS.charAt(charIndex);
    if (ch === ' ') ch = 'SPC';
    print(4, 28, "Char: " + ch, LightGrey);

    print(4, 40, "[Jog]scroll [Click]add", LightGrey);
    print(4, 50, "[Rec]start [Back]del", LightGrey);
}

function drawSampling() {
    var progress = host_module_get_param('progress') || '0/0';
    var noteName = host_module_get_param('current_note_name') || '--';
    var velocity = host_module_get_param('current_velocity') || '--';
    var status   = host_module_get_param('status') || '';

    print(4, 0, "SAMPLING:", White);
    print(4, 9, instrumentName, LightGrey);

    /* Parse progress "N/M" */
    var parts = progress.split('/');
    var done = parseInt(parts[0]) || 0;
    var total = parseInt(parts[1]) || 1;
    var barW = 100;
    var filled = Math.floor(barW * done / total);

    draw_rect(4, 22, barW, 8, White);
    if (filled > 0) fill_rect(4, 22, filled, 8, White);
    print(108, 22, progress, LightGrey);

    print(4, 34, "Note:" + noteName + " Vel:" + velocity, White);
    print(4, 44, status, LightGrey);
    print(4, 56, "[Hold Back to cancel]", LightGrey);

    /* Auto-transition */
    var state = host_module_get_param('state') || '';
    if (state === 'processing') {
        currentView = VIEW_PROCESSING;
    } else if (state === 'done') {
        currentView = VIEW_DONE;
    }
}

function drawProcessing() {
    var progress = host_module_get_param('progress') || '0/0';
    var status   = host_module_get_param('status') || '';

    print(4, 0, "PROCESSING:", White);
    print(4, 9, instrumentName, LightGrey);

    var parts = progress.split('/');
    var done = parseInt(parts[0]) || 0;
    var total = parseInt(parts[1]) || 1;
    var barW = 100;
    var filled = Math.floor(barW * done / total);

    draw_rect(4, 22, barW, 8, White);
    if (filled > 0) fill_rect(4, 22, filled, 8, White);
    print(108, 22, progress, LightGrey);

    print(4, 34, "Finding loops...", White);
    print(4, 44, status, LightGrey);

    var state = host_module_get_param('state') || '';
    if (state === 'done') {
        currentView = VIEW_DONE;
    }
}

function drawDone() {
    var completed  = host_module_get_param('completed') || '0';
    var zones      = host_module_get_param('zones_used') || '0';
    var loops      = host_module_get_param('loops_found') || '0';
    var skipped    = host_module_get_param('skipped') || '0';

    print(4, 0, "COMPLETE!", White);
    print(4, 12, instrumentName, White);
    print(4, 24, completed + " samples, " + zones + " zones", LightGrey);
    print(4, 33, loops + " loops found", LightGrey);
    print(4, 42, skipped + " skipped", LightGrey);

    print(4, 54, "Ready in SFZ Player", White);
}

/* ══════════════════════════════════════════════
   MIDI HANDLERS
   ══════════════════════════════════════════════ */

function handleSetupMidi(cc, value) {
    if (cc === MoveMainKnob) {
        var delta = decodeDelta(value);
        selectedParam = clamp(selectedParam + delta, 0, params.length - 1);
    } else if (cc === MoveKnob1) {
        var delta = decodeDelta(value);
        var p = params[selectedParam];
        p.val = clamp(p.val + delta * p.step, p.min, p.max);
        /* Round to step precision for floats */
        p.val = Math.round(p.val / p.step) * p.step;
    } else if (cc === MoveRec && value > 0) {
        currentView = VIEW_NAMING;
        announce("Name instrument");
    }
}

function handleNamingMidi(cc, value) {
    if (cc === MoveMainKnob) {
        var delta = decodeDelta(value);
        charIndex = charIndex + delta;
        if (charIndex < 0) charIndex = CHARS.length - 1;
        if (charIndex >= CHARS.length) charIndex = 0;
    } else if (cc === MoveMainButton && value > 0) {
        instrumentName += CHARS.charAt(charIndex);
    } else if (cc === MoveBack && value > 0) {
        if (instrumentName.length > 0) {
            instrumentName = instrumentName.substring(0, instrumentName.length - 1);
        } else {
            currentView = VIEW_SETUP;
            announce("Setup");
        }
    } else if (cc === MoveRec && value > 0) {
        startSampling();
    }
}

function handleSamplingMidi(cc, value) {
    if (cc === MoveBack) {
        if (value > 0) {
            backHeldSince = Date.now();
        } else {
            backHeldSince = 0;
        }
        if (backHeldSince > 0 && (Date.now() - backHeldSince) > 1000) {
            host_module_set_param('stop', '1');
            backHeldSince = 0;
            currentView = VIEW_DONE;
        }
    }
}

function handleProcessingMidi(cc, value) {
    /* Processing is automatic, no user controls needed */
}

function handleDoneMidi(cc, value) {
    if (value > 0) {
        /* Any button press resets to setup */
        instrumentName = '';
        charIndex = 0;
        currentView = VIEW_SETUP;
        announce("Setup");
    }
}

/* ── Start sampling ── */
function startSampling() {
    if (instrumentName.length === 0) return;
    for (var i = 0; i < params.length; i++) {
        host_module_set_param(params[i].key, String(params[i].val));
    }
    host_module_set_param('instrument_name', instrumentName);
    host_module_set_param('start', '1');
    currentView = VIEW_SAMPLING;
    backHeldSince = 0;
    announce("Sampling started");
    debugLog("Sampling started: " + instrumentName);
}

/* ══════════════════════════════════════════════
   GLOBAL HOOKS
   ══════════════════════════════════════════════ */

globalThis.init = function() {
    debugLog("Sample Robot loaded");
};

globalThis.tick = function() {
    clear_screen();
    switch (currentView) {
        case VIEW_SETUP:      drawSetup(); break;
        case VIEW_NAMING:     drawNaming(); break;
        case VIEW_SAMPLING:   drawSampling(); break;
        case VIEW_PROCESSING: drawProcessing(); break;
        case VIEW_DONE:       drawDone(); break;
    }

    /* Check back-hold during sampling for cancel */
    if (currentView === VIEW_SAMPLING && backHeldSince > 0) {
        if ((Date.now() - backHeldSince) > 1000) {
            host_module_set_param('stop', '1');
            backHeldSince = 0;
            currentView = VIEW_DONE;
        }
    }
};

globalThis.onMidiMessageInternal = function(data) {
    if ((data[0] & 0xF0) !== 0xB0) return;  /* CC only */
    var cc = data[1];
    var value = data[2];

    if (cc === MoveShift) {
        shiftHeld = value > 0;
        return;
    }

    switch (currentView) {
        case VIEW_SETUP:      handleSetupMidi(cc, value); break;
        case VIEW_NAMING:     handleNamingMidi(cc, value); break;
        case VIEW_SAMPLING:   handleSamplingMidi(cc, value); break;
        case VIEW_PROCESSING: handleProcessingMidi(cc, value); break;
        case VIEW_DONE:       handleDoneMidi(cc, value); break;
    }
};
