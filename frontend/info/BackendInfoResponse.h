#pragma once

#include "BackendInfoRec.h"
#include "BackendInfoFilter.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <macgyver/DateTime.h>
#include <spine/Table.h>
#include <json/json.h>

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{

class BackendInfoResponse
{
public:
  using parser_t = std::function<std::shared_ptr<BackendInfoRec>(const Json::Value&, const std::string&)>;

  /**
   * @brief Constructor from a single backend server response JSON object
   */
  BackendInfoResponse(
    const Json::Value& jsonObject,
    parser_t recordFactory,
    const BackendInfoFilter& recordFilter,
    const std::string& timeFormat);

  /**
   * @brief Constructor: filters records from another BackendInfoResponse object
   */
  BackendInfoResponse(
    const BackendInfoResponse& other,
    const BackendInfoFilter& recordFilter);

  /**
   * @brief Constructor: merges multiple BackendInfoResponse objects keeping only
   *        common records for all responses
   */
  BackendInfoResponse(
    const std::vector<std::shared_ptr<BackendInfoResponse>>& responses);

  virtual ~BackendInfoResponse();

  std::set<std::string> get_producers() const;

  const std::vector<std::shared_ptr<BackendInfoRec>>& get_records(const std::string& producer) const;

  BackendInfoResponse filter_records(const BackendInfoFilter& recordFilter) const;

  std::unique_ptr<SmartMet::Spine::Table> to_table(const std::string& timeFormat = "iso") const;

  Json::Value as_json(const std::string& timeFormat = "iso") const;

  inline std::size_t get_summary_size() const { return summary_size; }

  inline void set_title(const std::string& theTitle) { title = theTitle; }

  inline std::string get_title() const { return title; }

private:
  /**
   * @brief Map of backend info records keyed by producer name
   */
  std::map<std::string, std::vector<std::shared_ptr<BackendInfoRec>>> records;

  std::size_t summary_size = 0;

  std::string title;
};

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet
