/* GymServer - the protocol a gym process serves on stdin/stdout. */

#ifndef GYM_SERVER_H
#define GYM_SERVER_H

/* One JSON object per line in each direction, paired by "id". A reply that
 * carries a frame or audio announces them in its JSON and is followed by their
 * raw bytes: the frame (width*height*3, RGB, top row first), then the audio
 * (frames*channels little-endian int16). Commands:
 *
 *   hello  what this process plays: versions, frame and audio formats, the
 *          song, the chart, the effective options, timing windows, prefs.
 *   reset  set up the stage and return its first frame and audio. Once.
 *   step   {"buttons": [left, down, up, right], "frame": true} advances one
 *          frame holding those buttons; returns the state, frame and audio.
 *   state  the current state, without advancing.
 *   close  reply, then exit.
 *
 * A failed command replies {"id", "ok": false, "kind", "error"}. Closing stdin
 * ends the process. */
namespace GymServer {

/* Keep the real stdout for the protocol; what the game itself prints there
 * (its log) is dropped. Call before anything can print. */
void ClaimStdout();

/* Answer commands until "close" or the end of stdin. */
void Serve();

}  // namespace GymServer

#endif  // GYM_SERVER_H
