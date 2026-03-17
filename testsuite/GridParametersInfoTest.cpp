#include "../frontend/info/BackendInfoResponse.h"
#include "../frontend/info/GridParametersInfoRec.h"
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
  const char* name = "GridParameters Info tester";
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

  std::shared_ptr<BackendInfoResponse> read_response(const std::string& filePath,
                                                     BackendInfoResponse::parser_t recordFactory,
                                                     const std::string& timeFormat)
  {
    Json::Value jsonObject = parse_json_file(filePath);
    return std::make_shared<BackendInfoResponse>(
        jsonObject, recordFactory, BackendInfoFilter(), timeFormat);
  }

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(BackendInfoTest)

BOOST_AUTO_TEST_CASE(parse_info_gridparameters_response_1)
{
  BOOST_TEST_MESSAGE("GridParametersInfoTest: parse example backend response without parameter filtration");
  Json::Value jsonObject = parse_json_file("data/gp01.json");

  BackendInfoResponse response(
      jsonObject,
      [](const Json::Value& jsonObject, const std::string& timeFormat)
      {
        return std::make_shared<GridParametersInfoRec>(jsonObject, timeFormat);
      },
      BackendInfoFilter(),
      "iso");

  BOOST_CHECK(true);
  std::ofstream outputFile("output/GridParametersInfoTest_parse_info_gridparameters_response_1.json");
  outputFile << response.as_json().toStyledString();
  outputFile.close();
  //std::cout << "Parsed producers:" << response.as_json() <<   std::endl;
}

BOOST_AUTO_TEST_CASE(parse_info_gridparameters_filter_1)
{
  BOOST_TEST_MESSAGE("GridParametersInfoTest: parse example backend response with single producer");
  Json::Value jsonObject = parse_json_file("data/gp01.json");

  BackendInfoResponse response(
      jsonObject,
      [](const Json::Value& jsonObject, const std::string& timeFormat)
      {
        return std::make_shared<GridParametersInfoRec>(jsonObject, timeFormat);
      },
      BackendInfoFilter("ECG", {}),
      "iso");

  BOOST_CHECK(true);
  std::ofstream outputFile("output/GridParametersInfoTest_parse_info_gridparameters_filter_1.json");
  outputFile << response.as_json().toStyledString();
  outputFile.close();
  //std::cout << "Parsed producers:" << response.as_json() <<   std::endl;
}


BOOST_AUTO_TEST_CASE(parse_info_qengine_response_3)
{
  const auto item_reader = [](const Json::Value& jsonObject, const std::string& timeFormat)
      { return std::make_shared<GridParametersInfoRec>(jsonObject, timeFormat); };

  BOOST_TEST_MESSAGE("GridParametersInfoTest: parse example backend response2 and merge them");
  const std::string producer = "ECM_PROB";
  std::vector<std::shared_ptr<BackendInfoResponse>> responses;
  responses.emplace_back(read_response("data/gp01.json", item_reader, "iso"));
  responses.emplace_back(read_response("data/gp02.json", item_reader, "iso"));
  responses.emplace_back(read_response("data/gp03.json", item_reader, "iso"));

  std::shared_ptr<BackendInfoResponse> response;

  BOOST_REQUIRE_NO_THROW(response = std::make_shared<BackendInfoResponse>(responses));

  BOOST_CHECK(true);
  std::ofstream outputFile("output/GridParametersInfoTest_parse_info_gridparameters_response_3.json");
  outputFile << response->as_json().toStyledString();
  outputFile.close();
}

BOOST_AUTO_TEST_SUITE_END()
