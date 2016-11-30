#include "Cache.h"

#include <string>
#include <iostream>

// TODO: This file is probably useless?

namespace SmartMet
{
bool Cache::update(const std::string& /* theKey */)
{
  // Lift the key to the top of the list agait
  return true;
}

bool Cache::insert(const std::string& theKey, const std::string& /* theValue */)
{
  // Insert a new entry or replace the value of the existing cache key

  // If new entry was created, compact the list to contain only
  // maximum amount of entries.

  // Lift the object as the first element on the LRU list
  // by calling update() method
  update(theKey);

  return true;
}

const std::string& Cache::query(const std::string& theKey)
{
  // See if there is an entry matching the key;

  // Lift the object as the first element on the LRU list
  // by calling update() method
  update(theKey);

  // Return the value associated with the key to requester.
  //
  // TODO: Need to consider a shared pointer as the cache entry might
  // get deleted if the entry falls out of the LRU.
  return theKey;
}

Cache::Cache()
{
  // Banner
  std::cout << "\t   + HTTP Plugin proxy cache initializing..." << std::endl;
}

Cache::~Cache()
{
  // Destructor
}
}
