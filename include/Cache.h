#pragma once

#include <string>

namespace SmartMet
{
class Cache
{
 private:
  bool update(const std::string& theKey);

 public:
  bool insert(const std::string& theKey, const std::string& theValue);
  const std::string& query(const std::string& theKey);

  // Constructor and destructor
  Cache();
  ~Cache();
};
}  // namespace SmartMet
