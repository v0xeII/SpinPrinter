// A small passive buzzer (piezo or magnetic, the kind without its own generator) between a free
// pin and GND. Settings → Sound: which pin, how loud, clicks on the buttons.
//
// It is meant to be heard rarely: a soft chime when a print is done, two low notes on an error, a
// chirp when SpinSlicer waits for a yes on the screen. Every note fades in and out (no click), the
// quiet level is a few percent of the full power, and a second sound within 1.5 s is dropped:
// nothing ever repeats or beeps until you press a button.
#pragma once

enum Sound { SND_TICK, SND_DONE, SND_ERR, SND_ASK, SND_TEST };
enum { SOUND_OFF, SOUND_QUIET, SOUND_LOUD };

void buzzer_init();
void buzzer_pin(int pin);              // -1 = no buzzer
void buzzer_play(int sound, int level);  // level SOUND_*; returns at once, the notes play in a task
