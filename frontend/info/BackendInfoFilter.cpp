#include "BackendInfoFilter.h"

using namespace SmartMet::Plugin::Frontend;

BackendInfoFilter::BackendInfoFilter(
    std::optional<std::string> producer,
    const std::vector<std::string>& parameters)

    : itsProducer(std::move(producer))
    , itsParameters(parameters)
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

  const auto& recordParameters = record.get_parameters();
  for (const auto& param : itsParameters)
  {
    if (std::find(recordParameters.begin(), recordParameters.end(), param) == recordParameters.end())
      return false;
  }

  return true;
}
