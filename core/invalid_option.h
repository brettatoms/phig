#pragma once

#include <stdexcept>

// An option value outside the set its field defines: an unknown media type, a
// thumbnail size of zero, an empty destination. Each engine's validate() throws it
// before the engine does any work, so a surface without cli/'s argv parser still
// cannot start an operation on input phig does not recognize (R73). libphig maps it
// to PHIG_ERR_INVALID_ARG.
class InvalidOption : public std::invalid_argument {
   public:
    using std::invalid_argument::invalid_argument;
};
