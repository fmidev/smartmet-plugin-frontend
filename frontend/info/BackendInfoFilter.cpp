#include "BackendInfoFilter.h"

using namespace SmartMet::Plugin::Frontend;

BackendInfoFilter::BackendInfoFilter(
    std::optional<std::string> producer,
    const std::vector<std::string>& parameters,
    bool all)

    : itsProducer(std::move(producer))
    , itsParameters(parameters)
    , itsAll(all)
{
}


BackendInfoFilter::~BackendInfoFilter() = default;

bool BackendInfoFilter::operator()(const BackendInfoRec& record) const
{
  if (itsProducer)
  {
    if (record.get_producer() != *itsProducer)
      return false;
  }

  const bool result = record.contains_parameters(itsParameters, itsAll);
  return result;
}
