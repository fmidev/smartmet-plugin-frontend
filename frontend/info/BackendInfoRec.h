#pragma once

#include <vector>
#include <string>
#include <json/value.h>
#include <optional>
#include <macgyver/DateTime.h>
#include <macgyver/TimeFormatter.h>

namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{

/**
 * @brief Base class for backend info records
 *
 * This class provides common functionality for all backend info record types.
 * Derived classes are used when parsing JSON format info requests from backend
 */
class BackendInfoRec
{
public:
  BackendInfoRec(const Json::Value& jsonObject, const std::string& timeFormat);
  virtual ~BackendInfoRec();
  virtual std::vector<std::string> as_vector() const = 0;
  virtual Json::Value as_json() const = 0;
  virtual bool operator < (const BackendInfoRec& other) const = 0;
  virtual const std::vector<std::string> get_names() const = 0;
  virtual const std::string& get_producer() const = 0;
  virtual const std::vector<std::string>& get_parameters() const = 0;

  std::string format_datetime(const Fmi::DateTime& dt) const;

protected:
  static std::string get_string_field(
    const Json::Value& jsonObject,
    const std::string& fieldName,
    const std::optional<std::string>& defaultValue = std::nullopt);

  Fmi::DateTime get_datetime_field(
    const Json::Value& jsonObject,
    const std::string& fieldName) const;

  int64_t get_integer_field(
    const Json::Value& jsonObject,
    const std::string& fieldName,
    const std::optional<int64_t>& defaultValue = std::nullopt) const;

  static std::vector<std::string>
  get_string_vector_field(
    const Json::Value& jsonObject,
    const std::string& fieldName,
    const std::string& separators);

  static std::vector<int64_t>
  get_integer_vector_field(
    const Json::Value& jsonObject,
    const std::string& fieldName,
    const std::string& separators);

private:
    const std::string time_format;
    std::shared_ptr<Fmi::TimeFormatter> time_formatter;
};

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet