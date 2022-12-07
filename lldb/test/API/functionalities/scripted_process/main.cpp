#include <thread>

#include "baz.h"

std::condition_variable cv;
std::mutex mutex;
bool done;

int bar(int i) {
  int j = i * i;
  return j;
}

int foo(int i) { return bar(i); }

void compute_pow(int &n) {
  std::unique_lock<std::mutex> lock(mutex);
  cv.notify_one();
  n = foo(n);
  done = true;
  cv.notify_one();
}

void call_and_wait(int &n, std::mutex &mutex, std::condition_variable &cv) {
  while (baz(n, mutex, cv, done) != 42) {
    ;
  }
}

int main() {
  int n = 42;
  done = false;
  std::thread thread_1(call_and_wait, std::ref(n), std::ref(mutex),
                       std::ref(cv));
  std::thread thread_2(compute_pow, std::ref(n));

  thread_1.join();
  thread_2.join();

  return 0;
}
