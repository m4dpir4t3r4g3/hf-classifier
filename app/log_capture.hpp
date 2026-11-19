// The program's own messages (stdout and stderr) for HFClassifier.exe, which
// has no console: they go through a pipe that a thread of this process
// reads, writing each piece to the log file at once and keeping the last
// part in memory for the diagnostic report. (Redirecting stdout to the
// file with freopen left the file empty for readers while the program ran,
// on a user's PC and in CI.)
#pragma once

#include <string>

namespace hfc {

// Starts capturing into `path` (a new file; the old one becomes
// `previousPath`). Windows only; false elsewhere or if it cannot.
bool startOutputCapture(const std::string& path, const std::string& previousPath);
// Everything written so far is in the file; the capture ends (at exit).
void stopOutputCapture();
// The last messages captured (at most 2 MB), "" if not capturing.
std::string capturedOutput();

}  // namespace hfc
