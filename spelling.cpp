#include <iostream>
#include <fstream>
#include <vector>
#include <iterator>
#include <string>
#include <algorithm>
#include <bitset>

int main(int argc, char** argv) {
  std::string name = (argc > 1) ? argv[1] : "/usr/share/dict/words";
  std::ifstream fs;
  std::istream& file = name == "-" ? std::cin : (fs.open(name), fs);
  if (!file) {
    return std::cerr << "file open failed: \"" << name << "\"\n", 1;
  }
  std::vector<unsigned> sevens; sevens.reserve(1<<14);
  std::vector<unsigned> words; words.reserve(1<<15);
  std::bitset<32> word; int len = 0; int ones = 0;
  for (std::istreambuf_iterator<char> in(file), eof; in != eof; ++in) {
    if (*in == '\n') {
      if (len >= 5 && ones <= 7) {
	(ones == 7 ? sevens : words).push_back(word.to_ulong());
      }
      word = len = ones = 0;
    }
    else if (ones != 8 && *in >= 'a' && *in <= 'z') {
      ++len, ones = word.set(25 - (*in - 'a')).count();
    }
    else {
      ones = 8;
    }
