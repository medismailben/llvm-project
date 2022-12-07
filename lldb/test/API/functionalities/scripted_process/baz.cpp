#include "baz.h"

#include <math.h>

int baz(int j, std::mutex &mutex, std::condition_variable &cv, bool &done) {
  std::unique_lock<std::mutex> lock(mutex);
  cv.wait(lock, [&done] { return done; });
  int k = sqrt(j);
  return k; // break here
}
