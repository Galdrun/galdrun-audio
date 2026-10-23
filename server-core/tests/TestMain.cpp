#include "TestSupport.h"

// Arguments run only the tests whose name contains one of them.
int main(int argc, char** argv)
{
    return multiroomtest::runAll(std::vector<std::string>(argv + 1, argv + argc));
}
