#include <iostream>
#include <map>

int main()
{
   std::string name = "test";
   std::map<int, std::string> map{{1, "one"}, {2, "tow"}, {3, "three"}};
   std::cout << "Hello, " << name << "!" << '\n';
}
