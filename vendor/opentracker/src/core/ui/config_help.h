// Config help box texts (spec 15 §2, §3.2 item 1; layout spec 06 §7.2).
// Stock shows a help string for 25 options while their value picker is open;
// the wording here is OpenTracker's own (the stock texts are not copied).
#pragma once

namespace ot {

// Help text for option `opt` of Config category `cat` (spec 15 §2 order),
// or nullptr when stock has no help for that option.
const char* configHelp(int cat, int opt);

// Number of options with a help text (spec 15 §3.2: 25).
int configHelpCount();

}  // namespace ot
