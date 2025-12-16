#include "../frontend/info/BackendInfoRec.h"
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
  const char* name = "Parameter Lookup tester";
  std::filesystem::create_directory("output");
  unit_test_log.set_threshold_level(log_messages);
  framework::master_test_suite().p_name.value = name;
  BOOST_TEST_MESSAGE("");
  BOOST_TEST_MESSAGE(name);
  BOOST_TEST_MESSAGE(std::string(std::strlen(name), '='));
  return nullptr;
}

BOOST_AUTO_TEST_SUITE(ParameterLookupTests)

// Test lookup_parameter with single parameter list
BOOST_AUTO_TEST_CASE(lookup_parameter_single_list)
{
  std::vector<std::string> params1 = {"Temperature", "Pressure", "Humidity"};
  
  // Parameter found
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Temperature", params1));
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Pressure", params1));
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Humidity", params1));
  
  // Parameter not found
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("WindSpeed", params1));
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("", params1));
}

// Test lookup_parameter with multiple parameter lists
BOOST_AUTO_TEST_CASE(lookup_parameter_multiple_lists)
{
  std::vector<std::string> params1 = {"Temperature", "Pressure"};
  std::vector<std::string> params2 = {"WindSpeed", "WindDirection"};
  std::vector<std::string> params3 = {"Humidity", "DewPoint"};
  
  // Parameter found in first list
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Temperature", params1, params2, params3));
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Pressure", params1, params2, params3));
  
  // Parameter found in second list
  BOOST_CHECK(BackendInfoRec::lookup_parameter("WindSpeed", params1, params2, params3));
  BOOST_CHECK(BackendInfoRec::lookup_parameter("WindDirection", params1, params2, params3));
  
  // Parameter found in third list
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Humidity", params1, params2, params3));
  BOOST_CHECK(BackendInfoRec::lookup_parameter("DewPoint", params1, params2, params3));
  
  // Parameter not found in any list
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("Precipitation", params1, params2, params3));
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("CloudCover", params1, params2, params3));
}

// Test lookup_parameter with empty lists
BOOST_AUTO_TEST_CASE(lookup_parameter_empty_lists)
{
  std::vector<std::string> empty1;
  std::vector<std::string> empty2;
  std::vector<std::string> params = {"Temperature"};
  
  // Empty parameter lists
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("Temperature", empty1));
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("Temperature", empty1, empty2));
  
  // Mix of empty and non-empty lists
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Temperature", empty1, params));
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Temperature", params, empty1));
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("Pressure", empty1, params));
}

// Test lookup_parameter with no parameter lists (compile-time)
BOOST_AUTO_TEST_CASE(lookup_parameter_no_lists)
{
  // Should always return false when no parameter lists provided
  BOOST_CHECK(!BackendInfoRec::lookup_parameter<>("Temperature"));
  BOOST_CHECK(!BackendInfoRec::lookup_parameter<>(""));
}

// Test lookup_parameters with all=true (all parameters must match)
BOOST_AUTO_TEST_CASE(lookup_parameters_all_true)
{
  std::vector<std::string> params1 = {"Temperature", "Pressure", "Humidity"};
  std::vector<std::string> params2 = {"WindSpeed", "WindDirection"};
  
  // All search parameters found
  std::vector<std::string> search1 = {"Temperature", "Pressure"};
  BOOST_CHECK(BackendInfoRec::lookup_parameters(search1, true, params1));
  
  std::vector<std::string> search2 = {"Temperature", "WindSpeed"};
  BOOST_CHECK(BackendInfoRec::lookup_parameters(search2, true, params1, params2));
  
  // Not all search parameters found
  std::vector<std::string> search3 = {"Temperature", "CloudCover"};
  BOOST_CHECK(!BackendInfoRec::lookup_parameters(search3, true, params1));
  
  std::vector<std::string> search4 = {"Temperature", "Precipitation"};
  BOOST_CHECK(!BackendInfoRec::lookup_parameters(search4, true, params1, params2));
  
  // Single parameter search
  std::vector<std::string> search5 = {"Temperature"};
  BOOST_CHECK(BackendInfoRec::lookup_parameters(search5, true, params1));
  
  std::vector<std::string> search6 = {"Unknown"};
  BOOST_CHECK(!BackendInfoRec::lookup_parameters(search6, true, params1));
}

// Test lookup_parameters with all=false (any parameter must match)
BOOST_AUTO_TEST_CASE(lookup_parameters_all_false)
{
  std::vector<std::string> params1 = {"Temperature", "Pressure", "Humidity"};
  std::vector<std::string> params2 = {"WindSpeed", "WindDirection"};
  
  // At least one parameter found
  std::vector<std::string> search1 = {"Temperature", "CloudCover"};
  BOOST_CHECK(BackendInfoRec::lookup_parameters(search1, false, params1));
  
  std::vector<std::string> search2 = {"Unknown", "WindSpeed"};
  BOOST_CHECK(BackendInfoRec::lookup_parameters(search2, false, params1, params2));
  
  std::vector<std::string> search3 = {"Temperature", "Pressure"};
  BOOST_CHECK(BackendInfoRec::lookup_parameters(search3, false, params1));
  
  // No parameters found
  std::vector<std::string> search4 = {"CloudCover", "Precipitation"};
  BOOST_CHECK(!BackendInfoRec::lookup_parameters(search4, false, params1));
  
  std::vector<std::string> search5 = {"Unknown1", "Unknown2"};
  BOOST_CHECK(!BackendInfoRec::lookup_parameters(search5, false, params1, params2));
}

// Test lookup_parameters with empty search list
BOOST_AUTO_TEST_CASE(lookup_parameters_empty_search)
{
  std::vector<std::string> params = {"Temperature", "Pressure"};
  std::vector<std::string> empty_search;
  
  // Empty search list means no filtering - should return true (accept all)
  BOOST_CHECK(BackendInfoRec::lookup_parameters(empty_search, true, params));
  BOOST_CHECK(BackendInfoRec::lookup_parameters(empty_search, false, params));
}

// Test lookup_parameters with empty parameter lists
BOOST_AUTO_TEST_CASE(lookup_parameters_empty_param_lists)
{
  std::vector<std::string> search = {"Temperature", "Pressure"};
  std::vector<std::string> empty1;
  std::vector<std::string> empty2;
  
  // Empty parameter lists should return false
  BOOST_CHECK(!BackendInfoRec::lookup_parameters(search, true, empty1));
  BOOST_CHECK(!BackendInfoRec::lookup_parameters(search, false, empty1));
  BOOST_CHECK(!BackendInfoRec::lookup_parameters(search, true, empty1, empty2));
  BOOST_CHECK(!BackendInfoRec::lookup_parameters(search, false, empty1, empty2));
}

// Test lookup_parameters with no parameter lists
BOOST_AUTO_TEST_CASE(lookup_parameters_no_lists)
{
  std::vector<std::string> search = {"Temperature"};
  
  // No parameter lists should return false
  BOOST_CHECK(!BackendInfoRec::lookup_parameters<>(search, true));
  BOOST_CHECK(!BackendInfoRec::lookup_parameters<>(search, false));
}

// Test lookup_parameters with mixed scenarios
BOOST_AUTO_TEST_CASE(lookup_parameters_mixed_scenarios)
{
  std::vector<std::string> params1 = {"Temperature", "Pressure"};
  std::vector<std::string> params2 = {"WindSpeed"};
  std::vector<std::string> params3 = {"Humidity", "DewPoint", "Precipitation"};
  
  // All parameters from different lists (all=true)
  std::vector<std::string> search1 = {"Temperature", "WindSpeed", "Humidity"};
  BOOST_CHECK(BackendInfoRec::lookup_parameters(search1, true, params1, params2, params3));
  
  // Some parameters missing (all=true)
  std::vector<std::string> search2 = {"Temperature", "WindSpeed", "CloudCover"};
  BOOST_CHECK(!BackendInfoRec::lookup_parameters(search2, true, params1, params2, params3));
  
  // At least one parameter found (all=false)
  std::vector<std::string> search3 = {"Unknown1", "Unknown2", "Temperature"};
  BOOST_CHECK(BackendInfoRec::lookup_parameters(search3, false, params1, params2, params3));
  
  // All parameters not found (all=false)
  std::vector<std::string> search4 = {"Unknown1", "Unknown2", "Unknown3"};
  BOOST_CHECK(!BackendInfoRec::lookup_parameters(search4, false, params1, params2, params3));
}

// Test with case sensitivity
BOOST_AUTO_TEST_CASE(lookup_parameter_case_sensitive)
{
  std::vector<std::string> params = {"Temperature", "Pressure", "HUMIDITY"};
  
  // Exact match required (case-sensitive)
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Temperature", params));
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("temperature", params));
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("TEMPERATURE", params));
  
  BOOST_CHECK(BackendInfoRec::lookup_parameter("HUMIDITY", params));
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("humidity", params));
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("Humidity", params));
}

// Test with special characters and spaces
BOOST_AUTO_TEST_CASE(lookup_parameter_special_chars)
{
  std::vector<std::string> params = {"Temp-2m", "Wind Speed", "Pressure_MSL", "Rain/Snow"};
  
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Temp-2m", params));
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Wind Speed", params));
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Pressure_MSL", params));
  BOOST_CHECK(BackendInfoRec::lookup_parameter("Rain/Snow", params));
  
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("Temp 2m", params));
  BOOST_CHECK(!BackendInfoRec::lookup_parameter("WindSpeed", params));
}

BOOST_AUTO_TEST_SUITE_END()
