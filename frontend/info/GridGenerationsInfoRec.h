#pragma once

#include "BackendInfoRec.h"


namespace SmartMet
{
namespace Plugin
{
namespace Frontend
{

/* Sample:
{
    "ProducerName": "ECM_PROB",
    "GeometryId": 1007,
    "Timesteps": 85,
    "AnalysisTime": "20251204T120000",
    "MinTime": "20251204T120000",
    "MaxTime": "20251219T120000",
    "ModificationTime": "20251204T203635",
    "FmiParameters": "ENSMEMB-N,F0-FFG-MS,F0-RR-120-KGM2,F0-RR-24-KGM2,F10-FFG-MS,F10-RR-120-KGM2,F10-RR-24-KGM2,F100-FFG-MS,F100-RR-120-KGM2,F100-RR-24-KGM2,F25-FFG-MS,F25-RR-120-KGM2,F25-RR-24-KGM2,F50-FFG-MS,F50-RR-120-KGM2,F50-RR-24-KGM2,F75-FFG-MS,F75-RR-120-KGM2,F75-RR-24-KGM2,F90-FFG-MS,F90-RR-120-KGM2,F90-RR-24-KGM2,FFG-MEAN-MS,FFG-STDDEV-MS,PROB-CONV-RR3-1,PROB-CONV-RR3-2,PROB-CONV-RR3-3,PROB-CONV-RR3-4,PROB-CONV-RR3-5,PROB-CONV-RR3-6,PROB-RR-1,PROB-RR-2,PROB-RR-3,PROB-RR-4,PROB-RR12-1,PROB-RR12-2,PROB-RR12-3,PROB-RR24-1,PROB-RR24-2,PROB-RR24-3,PROB-RR24-4,PROB-RR24-5,PROB-RR24-6,PROB-RR24-7,PROB-RR3-1,PROB-RR3-2,PROB-RR3-3,PROB-RR3-4,PROB-RR3-5,PROB-RR3-6,PROB-TC-0,PROB-TC-1,PROB-TC-2,PROB-TC-3,PROB-TC-4,PROB-TC-5,PROB-TW-1,PROB-TW-2,PROB-TW-3,PROB-W-1,PROB-W-2,PROB-W-3,PROB-W-4,PROB-W-5,PROB-W-6,PROB-WG-1,PROB-WG-2,PROB-WG-3,PROB-WG-4,PROB-WG-5",
    "ParameterAliases": "AvailableEnsembleMemberCount,CyanosSumChange,CyanosSumChangeP,FrostProbability,Precipitation120hF0,Precipitation120hF10,Precipitation120hF100,Precipitation120hF25,Precipitation120hF50,Precipitation120hF75,Precipitation120hF90,Precipitation24hF0,Precipitation24hF10,Precipitation24hF100,Precipitation24hF25,Precipitation24hF50,Precipitation24hF75,Precipitation24hF90,ProbabilityOfColdLimit1,ProbabilityOfColdLimit2,ProbabilityOfColdLimit3,ProbabilityOfColdLimit4,ProbabilityOfColdLimit5,ProbabilityOfConvectivePrecipitationLimit1,ProbabilityOfConvectivePrecipitationLimit2,ProbabilityOfConvectivePrecipitationLimit3,ProbabilityOfConvectivePrecipitationLimit4,ProbabilityOfConvectivePrecipitationLimit5,ProbabilityOfConvectivePrecipitationLimit6,ProbabilityOfGustLimit1,ProbabilityOfGustLimit2,ProbabilityOfGustLimit3,ProbabilityOfGustLimit4,ProbabilityOfGustLimit5,ProbabilityOfHeatLimit1,ProbabilityOfHeatLimit2,ProbabilityOfHeatLimit3,ProbabilityOfPr24Limit1,ProbabilityOfPr24Limit2,ProbabilityOfPr24Limit3,ProbabilityOfPr24Limit4,ProbabilityOfPr24Limit5,ProbabilityOfPr24Limit6,ProbabilityOfPr24Limit7,ProbabilityOfPrecLimit1,ProbabilityOfPrecLimit2,ProbabilityOfPrecLimit3,ProbabilityOfPrecLimit4,ProbabilityOfPrecipitation3h01mm,ProbabilityOfPrecipitation3h05mm,ProbabilityOfPrecipitation3h0mm,ProbabilityOfPrecipitation3h2mm,ProbabilityOfWindLimit1,ProbabilityOfWindLimit2,ProbabilityOfWindLimit3,ProbabilityOfWindLimit4,ProbabilityOfWindLimit5,ProbabilityOfWindLimit6,Reflectivity,WindGustF0,WindGustF10,WindGustF100,WindGustF25,WindGustF50,WindGustF75,WindGustF90"
  },
*/

class GridGenerationsInfoRec : public BackendInfoRec
{
public:
    const std::string producer;
    int geometryId;
    int timesteps;
    Fmi::DateTime analysisTime;
    Fmi::DateTime minTime;
    Fmi::DateTime maxTime;
    Fmi::DateTime modificationTime;
    std::vector<std::string> fmiParameter;
    std::vector<std::string> parameterAliases;

    GridGenerationsInfoRec(const Json::Value& jsonObject, const std::string& timeFormat);

    std::vector<std::string> as_vector() const override;

    Json::Value as_json() const override;

    const std::vector<std::string> get_names() const override;

    bool operator < (const BackendInfoRec& other) const override;

    const std::string& get_producer() const override;

    const std::vector<std::string>& get_parameters() const override;

    bool contains_parameters(const std::vector<std::string>& parameters, bool all = true) const override;

    ~GridGenerationsInfoRec() override;
};

}  // namespace Frontend
}  // namespace Plugin
}  // namespace SmartMet