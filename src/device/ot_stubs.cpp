// polyend-doom -- the parts of OpenTracker's core the HAL refers to but this
// image does not link (app/project_io.cpp: the background job profile that
// board.cpp installs a cycle counter for and logs).

#include "app/project_io.h"

namespace ot {

const JobProfile& jobProfile() {
    static JobProfile none;
    return none;
}

void setJobProfileCycles(uint32_t (*cycles)(), uint32_t cyclesPerUs) {
    (void)cycles;
    (void)cyclesPerUs;
}

}  // namespace ot
