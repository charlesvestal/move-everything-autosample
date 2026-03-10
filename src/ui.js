import {
    MidiCC,
    MoveShift, MoveMainKnob, MoveMainButton, MoveBack,
    MoveCapture, MoveRec,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    White, Black, DarkGrey, LightGrey
} from '/data/UserData/move-anything/shared/constants.mjs';

import { decodeDelta } from '/data/UserData/move-anything/shared/input_filter.mjs';
import { announce } from '/data/UserData/move-anything/shared/screen_reader.mjs';
import { log as uniLog } from '/data/UserData/move-anything/shared/logger.mjs';

function debugLog(msg) { uniLog("SampleRobot", msg); }

globalThis.onModuleLoad = function() {
    debugLog("Sample Robot loaded");
};

globalThis.onDspReady = function() {
    debugLog("DSP ready");
};

globalThis.onTick = function() {
    clear_screen();
    print(4, 4, "SAMPLE ROBOT", White);
    print(4, 20, "Module loaded", LightGrey);
    print(4, 36, "Setup coming soon", LightGrey);
};

globalThis.onMidi = function(data) {
};
