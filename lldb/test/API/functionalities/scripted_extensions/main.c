struct Pair {
  int first;
  int second;
};

int main() {
  struct Pair pair = {11, 22};
  return pair.first + pair.second - 33; // break here
}
