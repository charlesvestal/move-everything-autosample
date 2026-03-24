import {
    MoveShift, MoveRec, MoveBack, MoveMainButton,
    White, Black, LightGrey
} from '/data/UserData/schwung/shared/constants.mjs';

import { decodeDelta, isCapacitiveTouchMessage } from '/data/UserData/schwung/shared/input_filter.mjs';
import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';
import { log as uniLog } from '/data/UserData/schwung/shared/logger.mjs';

/* Shared menu system */
import { createValue, createToggle, createAction, formatItemValue } from '/data/UserData/schwung/shared/menu_items.mjs';
import { createMenuState, handleMenuInput } from '/data/UserData/schwung/shared/menu_nav.mjs';
import { createMenuStack } from '/data/UserData/schwung/shared/menu_stack.mjs';
import { drawHierarchicalMenu } from '/data/UserData/schwung/shared/menu_render.mjs';

/* Shared text entry */
import {
    openTextEntry, isTextEntryActive, handleTextEntryMidi,
    drawTextEntry, tickTextEntry
} from '/data/UserData/schwung/shared/text_entry.mjs';

function debugLog(msg) { uniLog("AutoSample", msg); }

/* ── Views ── */
var VIEW_SETUP      = 0;
var VIEW_SAMPLING   = 1;
var VIEW_PROCESSING = 2;
var VIEW_DONE       = 3;

var currentView = VIEW_SETUP;
var shiftHeld = false;
var cancelled = false;
var lastAnnouncedProgress = '';
var doneAnnounced = false;

/* ── Setup params (mutable state) ── */
var NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];

function noteNameFmt(n) {
    return NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 2);
}

var config = {
    range_low: 36,
    range_high: 84,
    key_zones: 2,
    velocity_layers: 2,
    hold_duration: 3.0,
    loop_detect: true,
    midi_channel: 1
};

var instrumentName = '';

/* ── Menu system ── */
var menuState = createMenuState();
var menuStack = createMenuStack();

function buildSetupMenu() {
    return [
        createValue('Low Note', {
            get: function() { return config.range_low; },
            set: function(v) { config.range_low = v; },
            min: 0, max: 127, step: 1,
            format: noteNameFmt
        }),
        createValue('Hi Note', {
            get: function() { return config.range_high; },
            set: function(v) { config.range_high = v; },
            min: 0, max: 127, step: 1,
            format: noteNameFmt
        }),
        createValue('Zones', {
            get: function() { return config.key_zones; },
            set: function(v) { config.key_zones = v; },
            min: 1, max: 24, step: 1
        }),
        createValue('Layers', {
            get: function() { return config.velocity_layers; },
            set: function(v) { config.velocity_layers = v; },
            min: 1, max: 8, step: 1
        }),
        createValue('Hold (s)', {
            get: function() { return config.hold_duration; },
            set: function(v) { config.hold_duration = Math.round(v * 2) / 2; },
            min: 0.5, max: 30, step: 0.5,
            format: function(v) { return v.toFixed(1); }
        }),
        createToggle('Loop Detect', {
            get: function() { return config.loop_detect; },
            set: function(v) { config.loop_detect = v; }
        }),
        createValue('MIDI Ch', {
            get: function() { return config.midi_channel; },
            set: function(v) { config.midi_channel = v; },
            min: 1, max: 16, step: 1
        }),
        createAction('Name & Sample', function() {
            openTextEntry({
                title: 'Instrument Name',
                initialText: instrumentName,
                onConfirm: function(text) {
                    instrumentName = text;
                    if (instrumentName.length > 0) {
                        startSampling();
                    }
                },
                onCancel: function() {
                    announce("Cancelled");
                }
            });
        })
    ];
}

/* ── Back-hold tracking for cancel ── */
var backHeldSince = 0;

/* ── Helper: total sample count ── */
function totalSamples() {
    return config.key_zones * config.velocity_layers;
}

/* ══════════════════════════════════════════════
   DRAW FUNCTIONS (sampling/processing/done only)
   ══════════════════════════════════════════════ */

function drawSampling() {
    var progress = host_module_get_param('progress') || '0/0';
    var noteName = host_module_get_param('current_note_name') || '--';
    var velocity = host_module_get_param('current_velocity') || '--';
    var status   = host_module_get_param('status') || '';

    print(4, 0, "SAMPLING:", White);
    print(4, 9, instrumentName, LightGrey);

    var parts = progress.split('/');
    var done = parseInt(parts[0]) || 0;
    var total = parseInt(parts[1]) || 1;
    var barW = 100;
    var filled = Math.floor(barW * done / total);

    draw_rect(4, 22, barW, 8, White);
    if (filled > 0) fill_rect(4, 22, filled, 8, White);
    print(108, 22, progress, LightGrey);

    print(4, 34, "Note: " + noteName + "  Vel: " + velocity, White);
    print(4, 44, status, LightGrey);
    print(4, 56, "[Hold Back to cancel]", LightGrey);

    /* Announce each new sample */
    if (progress !== lastAnnouncedProgress) {
        lastAnnouncedProgress = progress;
        announce(noteName + " velocity " + velocity + ", " + progress);
    }

    /* Auto-transition (only fires once since currentView changes) */
    var state = host_module_get_param('state') || '';
    if (state === 'processing' && currentView === VIEW_SAMPLING) {
        currentView = VIEW_PROCESSING;
        announce("Processing");
    } else if (state === 'done' && currentView === VIEW_SAMPLING) {
        currentView = VIEW_DONE;
        announce("Complete");
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
    if (state === 'done' && currentView === VIEW_PROCESSING) {
        currentView = VIEW_DONE;
        announce("Complete");
    }
}

function drawDone() {
    var completed  = host_module_get_param('completed') || '0';
    var zones      = host_module_get_param('zones_used') || '0';
    var loops      = host_module_get_param('loops_found') || '0';
    var skipped    = host_module_get_param('skipped') || '0';
    var wasCancelled = cancelled;

    if (wasCancelled) {
        print(4, 0, "CANCELLED", White);
        print(4, 12, instrumentName, LightGrey);
        if (parseInt(completed) > 0) {
            print(4, 28, completed + " samples captured", LightGrey);
        } else {
            print(4, 28, "No samples saved", LightGrey);
        }
        print(4, 54, "[Press any button]", LightGrey);
        if (!doneAnnounced) {
            doneAnnounced = true;
            announce("Cancelled. " + (parseInt(completed) > 0 ? completed + " samples captured" : "No samples saved"));
        }
    } else {
        print(4, 0, "COMPLETE!", White);
        print(4, 12, instrumentName, White);
        print(4, 24, completed + " samples, " + zones + " zones", LightGrey);
        print(4, 33, loops + " loops found", LightGrey);
        print(4, 42, skipped + " skipped", LightGrey);
        print(4, 54, "Ready in SFZ Player", White);
        if (!doneAnnounced) {
            doneAnnounced = true;
            announce("Complete. " + completed + " samples, " + loops + " loops. Ready in SFZ Player");
        }
    }
}

/* ── Start sampling ── */
function startSampling() {
    if (instrumentName.length === 0) return;
    cancelled = false;
    doneAnnounced = false;
    lastAnnouncedProgress = '';
    var cfg = {
        range_low: config.range_low,
        range_high: config.range_high,
        key_zones: config.key_zones,
        velocity_layers: config.velocity_layers,
        hold_duration: config.hold_duration,
        loop_detect: config.loop_detect ? 1 : 0,
        midi_channel: config.midi_channel,
        instrument_name: instrumentName,
        start: 1
    };
    var json = JSON.stringify(cfg);
    debugLog("startSampling config: " + json);
    host_module_set_param('config_json', json);
    currentView = VIEW_SAMPLING;
    backHeldSince = 0;
    announce("Sampling started");
    debugLog("Sampling started: " + instrumentName);
}

/* ══════════════════════════════════════════════
   GLOBAL HOOKS
   ══════════════════════════════════════════════ */

globalThis.init = function() {
    debugLog("AutoSample loaded");
    menuStack.push({
        title: 'AUTOSAMPLE',
        items: buildSetupMenu()
    });
};

globalThis.tick = function() {
    clear_screen();

    if (isTextEntryActive()) {
        tickTextEntry();
        drawTextEntry();
    } else {
        switch (currentView) {
            case VIEW_SETUP:
                drawHierarchicalMenu({
                    title: 'AUTOSAMPLE',
                    items: menuStack.current() ? menuStack.current().items : buildSetupMenu(),
                    state: menuState,
                    footer: { left: 'Total: ' + totalSamples(), right: 'Rec: start' }
                });
                break;
            case VIEW_SAMPLING:   drawSampling(); break;
            case VIEW_PROCESSING: drawProcessing(); break;
            case VIEW_DONE:       drawDone(); break;
        }
    }

    /* Poll DSP for pending MIDI and send via JS API */
    if (currentView === VIEW_SAMPLING || currentView === VIEW_PROCESSING) {
        var pending = host_module_get_param('midi_pending');
        if (pending) {
            var parts = pending.split(',');
            var type = parseInt(parts[0]);
            var note = parseInt(parts[1]);
            var vel  = parseInt(parts[2]);
            var ch   = parseInt(parts[3]) || 0;
            if (type === 1) {
                move_midi_external_send([0x29, 0x90 | ch, note, vel]);
                debugLog("MIDI ON ch=" + ch + " n=" + note + " v=" + vel);
            } else if (type === 2) {
                move_midi_external_send([0x28, 0x80 | ch, note, 0]);
                debugLog("MIDI OFF ch=" + ch + " n=" + note);
            }
        }
    }

    /* Check back-hold during sampling for cancel */
    if (currentView === VIEW_SAMPLING && backHeldSince > 0) {
        if ((Date.now() - backHeldSince) > 1000) {
            host_module_set_param('stop', '1');
            cancelled = true;
            backHeldSince = 0;
            currentView = VIEW_DONE;
            announce("Cancelled");
        }
    }
};

globalThis.onMidiMessageInternal = function(data) {
    if (isCapacitiveTouchMessage(data)) return;

    var status = data[0] & 0xF0;
    var cc = data[1];
    var value = data[2];

    /* Track shift */
    if (status === 0xB0 && cc === MoveShift) {
        shiftHeld = value > 0;
        return;
    }

    /* Text entry gets priority */
    if (isTextEntryActive()) {
        handleTextEntryMidi(data);
        return;
    }

    /* Only handle CC from here */
    if (status !== 0xB0) return;

    switch (currentView) {
        case VIEW_SETUP: {
            /* Rec button as shortcut to start */
            if (cc === MoveRec && value > 0) {
                openTextEntry({
                    title: 'Instrument Name',
                    initialText: instrumentName,
                    onConfirm: function(text) {
                        instrumentName = text;
                        if (instrumentName.length > 0) {
                            startSampling();
                        }
                    },
                    onCancel: function() {
                        announce("Cancelled");
                    }
                });
                return;
            }
            var items = menuStack.current() ? menuStack.current().items : buildSetupMenu();
            handleMenuInput({
                cc: cc,
                value: value,
                items: items,
                state: menuState,
                stack: menuStack,
                shiftHeld: shiftHeld,
                onBack: function() { host_module_exit(); }
            });
            break;
        }
        case VIEW_SAMPLING:
            if (cc === MoveBack) {
                if (value > 0) {
                    backHeldSince = Date.now();
                } else {
                    backHeldSince = 0;
                }
            }
            break;
        case VIEW_PROCESSING:
            break;
        case VIEW_DONE:
            if (value > 0 && (cc === MoveBack || cc === MoveMainButton || cc === MoveRec)) {
                instrumentName = '';
                currentView = VIEW_SETUP;
                announce("Setup");
            }
            break;
    }
};
