// Link-boundary canary: this translation unit links paroculus-interact and then
// reaches for a Qt header. It MUST NOT COMPILE.
//
// The interact layer is toolkit-free and raster-free by construction: it
// consumes abstract input events and emits document commands, which is what
// makes gesture scripts runnable headlessly in CI. Nothing it links brings Qt's
// include directory, so a Qt include cannot resolve — unless Qt is linked into
// paroculus-interact, at which point this canary compiles and the layer has
// quietly grown a dependency on a display.
//
// CTest builds this target with WILL_FAIL. A green build here is the failure.
#include <QtCore/QObject>

int main() { return sizeof(QObject) > 0 ? 0 : 1; }
