#include "DataIngestionLayer.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, const char *argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <data-path>" << std::endl;
    return 64;
  }

  try {
    const std::string file_path = argv[1];

    std::cout << "Starting ingestion for: " << file_path << std::endl;
    const int result = RunDataIngestionFile(file_path);

    std::cout << "Ingestion finished with code: " << result << std::endl;
    return result;
  } catch (const std::exception &ex) {
    std::cerr << "Back-tester threw an exception: " << ex.what() << std::endl;
    return 1;
  }
}
