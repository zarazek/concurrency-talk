#include <thread>
#include <iostream>

void uncaughtException() {
  throw std::runtime_error("Some error");
}

int main(int, char**) {
  try {
    std::thread thr(uncaughtException);
    thr.join();
  } catch (std::exception& ex) {
    std::cout << "Exception: " << ex.what() << std::endl;
  }
  return 0;
}
