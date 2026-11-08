#pragma once

namespace gl_android_abi {
// On Apple arm64, adapt Android's 8-byte integer stack argument slots to
// Darwin's packed slots. Unknown names, null pointers and other hosts pass through.
void *wrap(const char *name, void *native);
}
