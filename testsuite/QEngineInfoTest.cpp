#include "../frontend/info/BackendInfoResponse.h"
#include "../frontend/info/QEngineInfoRec.h"
#include <boost/test/included/unit_test.hpp>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <macgyver/Exception.h>

using namespace boost::unit_test;
using namespace SmartMet::Plugin::Frontend;
using namespace std::string_literals;

test_suite* init_unit_test_suite(int argc, char* argv[])
{
  const char* name = "QEngine Info tester";
  std::filesystem::create_directory("output");
  unit_test_log.set_threshold_level(log_messages);
  framework::master_test_suite().p_name.value = name;
  BOOST_TEST_MESSAGE("");
  BOOST_TEST_MESSAGE(name);
  BOOST_TEST_MESSAGE(std::string(std::strlen(name), '='));
  return nullptr;
}

namespace
{
  Json::Value parse_json_file(const std::string& filePath)
  {
    std::ifstream inputFile(filePath);
    if (!inputFile.is_open())
      throw Fmi::Exception(BCP, "Failed to open JSON file: " + filePath);
    Json::Value jsonObject;
    Json::CharReaderBuilder readerBuilder;
    std::string errs;
    bool parsingSuccessful = Json::parseFromStream(readerBuilder, inputFile, &jsonObject, &errs);
    if (!parsingSuccessful)
      throw Fmi::Exception(BCP, "Failed to parse JSON file: " + filePath + " Error: " + errs);
    return jsonObject;
  }

  std::unique_ptr<BackendInfoResponse> read_response(const std::string& filePath,
                                                     BackendInfoResponse::parser_t recordFactory,
                                                     const std::string& timeFormat)
  {
    Json::Value jsonObject = parse_json_file(filePath);
    return std::make_unique<BackendInfoResponse>(
        jsonObject, recordFactory, BackendInfoFilter(), timeFormat);
  }

  std::vector<Fmi::DateTime> extract_origin_times(const BackendInfoResponse& response,
                                               const std::string& producer)
  {
    std::vector<Fmi::DateTime> originTimes;
    const auto& records = response.get_records(producer);
    for (const auto& recordPtr : records)
    {
      const auto& qrecord = dynamic_cast<const QEngineInfoRec&>(*recordPtr);
      originTimes.push_back(qrecord.originTime);
    }
    std::sort(originTimes.begin(), originTimes.end());
    return originTimes;
  }
} // anonymous namespace

BOOST_AUTO_TEST_SUITE(BackendInfoTest)

BOOST_AUTO_TEST_CASE(parse_info_qengine_response_1)
{
  BOOST_TEST_MESSAGE("QEngineInfoTest: parse example backend response without parameter filtration");
  Json::Value jsonObject = parse_json_file("data/q01.json");

  BackendInfoResponse response(
      jsonObject,
      [](const Json::Value& jsonObject, const std::string& timeFormat)
      {
        return std::make_shared<QEngineInfoRec>(jsonObject, timeFormat);
      },
      BackendInfoFilter(),
      "iso");

  BOOST_CHECK(true);
  std::ofstream outputFile("output/QEngineInfoTest_parse_info_qengine_response_1.json");
  outputFile << response.as_json().toStyledString();
  outputFile.close();
  //std::cout << "Parsed producers:" << response.as_json() <<   std::endl;
}

BOOST_AUTO_TEST_CASE(parse_info_qengine_response_2)
{
  BOOST_TEST_MESSAGE("QEngineInfoTest: parse example backend response with single parameter filtration");
  Json::Value jsonObject = parse_json_file("data/q01.json");

  BackendInfoResponse response(
      jsonObject,
      [](const Json::Value& jsonObject, const std::string& timeFormat)
      {
        return std::make_shared<QEngineInfoRec>(jsonObject, timeFormat);
      },
      BackendInfoFilter(std::nullopt, {"Temperature"}),
      "iso");

  BOOST_CHECK(true);
  std::ofstream outputFile("output/QEngineInfoTest_parse_info_qengine_response_2.json");
  outputFile << response.as_json().toStyledString();
  outputFile.close();
  //std::cout << "Parsed producers:" << response.as_json() <<   std::endl;
}

BOOST_AUTO_TEST_CASE(parse_info_qengine_response_3)
{
  const auto item_reader = [](const Json::Value& jsonObject, const std::string& timeFormat)
      { return std::make_shared<QEngineInfoRec>(jsonObject, timeFormat); };

  BOOST_TEST_MESSAGE("QEngineInfoTest: parse example backend responses and merge them");
  const std::string producer = "devmos";
  std::vector<std::unique_ptr<BackendInfoResponse>> responses;
  responses.emplace_back(read_response("data/q01.json", item_reader, "iso"));
  responses.emplace_back(read_response("data/q02.json", item_reader, "iso"));
  responses.emplace_back(read_response("data/q03.json", item_reader, "iso"));

  const auto ot1 = extract_origin_times(*responses[0], producer);
  // 2 items editted away in q02.json for DEVMOS
  const auto ot2 = extract_origin_times(*responses[1], producer);
  const auto ot3 = extract_origin_times(*responses[2], producer);
  BOOST_CHECK_EQUAL(ot1.size(), 5);
  BOOST_CHECK_EQUAL(ot2.size(), 3);
  BOOST_CHECK_EQUAL(ot3.size(), 5);

  std::unique_ptr<BackendInfoResponse> response;

  BOOST_REQUIRE_NO_THROW(response = std::make_unique<BackendInfoResponse>(responses));

  const auto otm = extract_origin_times(*response, producer);
  BOOST_CHECK_EQUAL(otm.size(), 3);
  BOOST_CHECK(otm == ot2);

  BOOST_CHECK_EQUAL(otm.at(0).to_iso_extended_string(), "2025-12-04T00:00:00"s);
  BOOST_CHECK_EQUAL(otm.at(1).to_iso_extended_string(), "2025-12-04T12:00:00"s);
  BOOST_CHECK_EQUAL(otm.at(2).to_iso_extended_string(), "2025-12-05T00:00:00"s);

  BOOST_CHECK(true);
  std::ofstream outputFile("output/QEngineInfoTest_parse_info_qengine_response_3.json");
  outputFile << response->as_json().toStyledString();
  outputFile.close();
}

BOOST_AUTO_TEST_SUITE_END()
