// Export / render cancel state (spec 05 appendix E1.4). The renders
// themselves are App members (App::exportAudio, App::exportSamples in
// render.cpp); while one runs, the bottom bar shows only F8 "Cancel" and the
// input queue is polled between audio blocks / sample chunks.
#pragma once

namespace ot {

// True when the last App::exportAudio / App::exportSamples was stopped with
// F8 "Cancel". Its partial output has been removed (OpenTracker; the spec
// does not say what stock leaves behind).
bool exportWasCancelled();

}  // namespace ot
