// app/currenttokens.cpp

#include "app/currenttokens.h"

namespace omega::app {
namespace {

// Function-local rather than a namespace-scope object: no static
// initialisation order to reason about, and specTokens() runs on the first
// call rather than before main().
theme::Tokens &store() {
    static theme::Tokens tokens = theme::specTokens();
    return tokens;
}

}  // namespace

const theme::Tokens &currentTokens() { return store(); }

void setCurrentTokens(const theme::Tokens &tokens) { store() = tokens; }

}  // namespace omega::app