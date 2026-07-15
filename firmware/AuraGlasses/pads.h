#ifndef AURA_PADS_H
#define AURA_PADS_H

// TTP223 capacitive touch pads on D5=GPIO5 and D6=GPIO6 (active HIGH,
// momentary). A 20 ms scan task debounces and turns touches into tap /
// hold gestures; each gesture enqueues the action configured in
// g_cfg.pad_act[slot]. Hold fires as soon as the threshold is reached
// (not at release) so a blind user gets immediate feedback.

void pads_init();  // call after actions_init()

#endif  // AURA_PADS_H
