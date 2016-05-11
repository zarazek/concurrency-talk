#include <functional>
#include <iostream>

using namespace std::placeholders;

void domesticAnimals(const std::string& owner, int quantity, const std::string& animal) {
  std::cout << owner << " has " << quantity << ' ' << animal << (quantity > 1 ? "s" : "") << std::endl;
}

int main(int, char**) {
  auto ownerSet = std::bind(domesticAnimals, "Olga", _1, _2);
  auto animalSet = std::bind(domesticAnimals, _1, _2, "cat");
  auto quantitySetAndOrderReversed = std::bind(domesticAnimals, _2, 3, _1);
  auto allSet = std::bind(domesticAnimals, "Kate", 2, "dog");

  domesticAnimals("Ala", 1, "cat");
  ownerSet(1, "spider");
  animalSet("Daniel", 2);
  quantitySetAndOrderReversed("snake", "Richard");
  allSet();
}
