#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

struct OrbitalParameters
{
	// Kepler orbital parameters
	double ecc, sma, inc, lan, arg, m0; // Meters, radians
	glm::dvec3 computePosition(double epoch, double parentGM);
};

struct AtmosphericParameters
{
	glm::vec4 K;
	float density;
	float maxHeight; /// Max atmospheric height
	float scaleHeight; /// Scale height
	bool hasAtmosphere;

	void generateLookupTable(std::vector<float> &table, size_t size, float radius) const;
};

struct RingParameters
{
	bool hasRings;
	float innerDistance; /// distance to planet of inner ring
	float outerDistance; /// distance to planet of outer ring
	glm::vec3 normal; /// Plane normal (normalized)

	void loadFile(std::string filename, std::vector<float> &pixelData) const;
};

struct BodyParameters
{
	glm::vec3 rotationAxis;
	float rotationPeriod; // radians per second
	glm::vec3 meanColor; // Color seen from far away
	float radius; // km
	double GM; // gravitational parameter
	bool isStar; // for lighting computation
	float brightness; // light intensity from far away
	float cloudDispPeriod;
	float nightTexIntensity;
};

struct AssetPaths
{
	std::string diffuseFilename;
	std::string cloudFilename;
	std::string nightFilename;
	std::string modelFilename;
	// Ring assets
	std::string backscatFilename;
	std::string forwardscatFilename;
	std::string unlitFilename;
	std::string transparencyFilename;
	std::string colorFilename;
};

// Immutable state
class PlanetParameters
{
public:
	std::string name;
	std::string parentName;
	OrbitalParameters orbitParam;
	AtmosphericParameters atmoParam;
	RingParameters ringParam;
	BodyParameters bodyParam;
	AssetPaths assetPaths;
};

// Mutable state
class PlanetState
{
public:
	glm::dvec3 position;
	float rotationAngle;
	float cloudDisp;
};