#pragma once

// save() serializes the candidate. Publishing it to loop-owned durable RAM is
// the last step; a failed or indeterminate save leaves that image untouched.
template<class Prefs, class Save>
bool commitObserverPreferenceCandidate(Prefs& durable, const Prefs* candidate, Save save) {
  if (!save()) return false;
  if (candidate) durable = *candidate;
  return true;
}
