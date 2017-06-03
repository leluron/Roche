#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <iostream>

#include "game.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>
#include <functional>
#include <memory>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <stdexcept>

#include "renderer.hpp"
#include "renderer_gl.hpp"

#include "thirdparty/shaun/sweeper.hpp"
#include "thirdparty/shaun/parser.hpp"

#include <glm/ext.hpp>

const float CAMERA_FOVY = 40.0;

std::string generateScreenshotName();

Game::Game()
{
	renderer.reset(new RendererGL());
}

Game::~Game()
{
	renderer->destroy();

	glfwTerminate();
}

std::string readFile(const std::string &filename)
{
	std::ifstream in(filename.c_str(), std::ios::in | std::ios::binary);
	if (!in) throw std::runtime_error("Can't open" + filename);
	std::string contents;
	in.seekg(0, std::ios::end);
	contents.resize(in.tellg());
	in.seekg(0, std::ios::beg);
	in.read(&contents[0], contents.size());
	in.close();
	return contents;
}

void Game::loadSettingsFile()
{
	using namespace shaun;
	try 
	{
		parser p{};
		const std::string fileContent = readFile("config/settings.sn");
		object obj = p.parse(fileContent.c_str());
		sweeper swp(&obj);

		sweeper video(swp("video"));
		auto fs = video("fullscreen");
		fullscreen = (fs.is_null())?true:(bool)fs.value<boolean>();

		if (!fullscreen)
		{
			width = video("width").value<number>();
			height = video("height").value<number>();
		}

		sweeper graphics(swp("graphics"));
		maxTexSize = graphics("maxTexSize").value<number>();
		msaaSamples = graphics("msaaSamples").value<number>();

		sweeper controls(swp("controls"));
		sensitivity = controls("sensitivity").value<number>();
	} 
	catch (parse_error &e)
	{
		std::cout << e << std::endl;
	}
}

void Game::init()
{
	loadSettingsFile();
	loadPlanetFiles();

	cameraPolar.z = planetParams[focusedPlanetId].bodyParam.radius*4;

	// Window & context creation
	if (!glfwInit())
		exit(-1);

	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	const GLFWvidmode* mode = glfwGetVideoMode(monitor);
	glfwWindowHint(GLFW_RED_BITS, mode->redBits);
	glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
	glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
	glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
	glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);
	renderer->windowHints();

	if (fullscreen)
	{
		width = mode->width;
		height = mode->height;
	}
	win = glfwCreateWindow(width, height, "Roche", fullscreen?monitor:nullptr, nullptr);

	if (!win)
	{
		glfwTerminate();
		exit(-1);
	}
	glfwMakeContextCurrent(win);

	glewExperimental = true;
	const GLenum err = glewInit();
	if (err != GLEW_OK)
	{
		throw std::runtime_error("Can't initialize GLEW : " + std::string((const char*)glewGetErrorString(err)));
	}

	// Renderer init
	renderer->init(planetParams, msaaSamples, maxTexSize, width, height);
}

template<class T>
T get(shaun::sweeper swp);

template <>
double get(shaun::sweeper swp)
{
	if (swp.is_null()) return 0.0; else return swp.value<shaun::number>();
}

template <>
std::string get(shaun::sweeper swp)
{
	if (swp.is_null()) return ""; else return std::string(swp.value<shaun::string>());
}

template <>
bool get(shaun::sweeper swp)
{
	if (swp.is_null()) return false; else return swp.value<shaun::boolean>();
}

template<>
glm::vec3 get(shaun::sweeper swp)
{
	glm::vec3 ret;
	if (swp.is_null()) return ret;
	for (int i=0;i<3;++i)
		ret[i] = swp[i].value<shaun::number>();
	return ret;
}

template<>
glm::vec4 get(shaun::sweeper swp)
{
	glm::vec4 ret;
	if (swp.is_null()) return ret;
	for (int i=0;i<4;++i)
		ret[i] = swp[i].value<shaun::number>();
	return ret;
}

void Game::loadPlanetFiles()
{
	using namespace shaun;
	try
	{
		parser p;
		std::string fileContent = readFile("config/planets.sn");
		object obj = p.parse(fileContent.c_str());
		sweeper swp(&obj);

		ambientColor = (float)get<double>(swp("ambientColor"));

		sweeper planetsSweeper(swp("planets"));
		planetCount = planetsSweeper.value<list>().elements().size();
		planetParams.resize(planetCount);
		planetStates.resize(planetCount);
		planetParents.resize(planetCount, -1);

		for (uint32_t i=0;i<planetCount;++i)
		{
			PlanetParameters planet = {};
			sweeper pl(planetsSweeper[i]);
			planet.name = std::string(pl("name").value<string>());
			planet.parentName = get<std::string>(pl("parent"));

			auto & orbitParam = planet.orbitParam;
			auto & bodyParam = planet.bodyParam;
			auto & atmoParam = planet.atmoParam;
			auto & assetPaths = planet.assetPaths;
			auto & ringParam = planet.ringParam;

			sweeper orbit(pl("orbit"));
			if (!orbit.is_null())
			{
				
				orbitParam.ecc = get<double>(orbit("ecc"));
				orbitParam.sma = get<double>(orbit("sma"));
				orbitParam.inc = glm::radians(get<double>(orbit("inc")));
				orbitParam.lan = glm::radians(get<double>(orbit("lan")));
				orbitParam.arg = glm::radians(get<double>(orbit("arg")));
				orbitParam.m0  = glm::radians(get<double>(orbit("m0" )));
			}
			sweeper body(pl("body"));
			if (!body.is_null())
			{
				bodyParam.radius = get<double>(body("radius"));
				float rightAscension = glm::radians(get<double>(body("rightAscension")));
				float declination = glm::radians(get<double>(body("declination")));
				bodyParam.rotationAxis = glm::vec3(
					-sin(rightAscension)*cos(declination),
					 cos(rightAscension)*cos(declination),
					 sin(declination));
				bodyParam.rotationPeriod = get<double>(body("rotPeriod"));
				bodyParam.meanColor = get<glm::vec3>(body("meanColor"));
				bodyParam.brightness = get<double>(body("brightness"));
				bodyParam.GM = get<double>(body("GM"));
				bodyParam.isStar = get<bool>(  body("isStar"));
				assetPaths.diffuseFilename = get<std::string>(body("diffuse"));
				assetPaths.nightFilename = get<std::string>(body("night"));
				assetPaths.cloudFilename = get<std::string>(body("cloud"));
				assetPaths.modelFilename = get<std::string>(body("model"));
				bodyParam.cloudDispPeriod = get<double>(body("cloudDispPeriod"));
				bodyParam.nightTexIntensity = get<double>(body("nightTexIntensity"));
			}
			sweeper atmo(pl("atmosphere"));
			atmoParam.hasAtmosphere = !atmo.is_null();
			if (!atmo.is_null())
			{
				atmoParam.hasAtmosphere = true;
				atmoParam.maxHeight = get<double>(atmo("maxAltitude"));
				atmoParam.K = get<glm::vec4>(atmo("K"));
				atmoParam.density = get<double>(atmo("density"));
				atmoParam.scaleHeight = get<double>(atmo("scaleHeight"));
			}

			sweeper ring(pl("ring"));
			ringParam.hasRings = !ring.is_null();
			if (!ring.is_null())
			{
				ringParam.innerDistance = get<double>(ring("inner"));
				ringParam.outerDistance = get<double>(ring("outer"));
				float rightAscension = glm::radians(get<double>(ring("rightAscension")));
				float declination = glm::radians(get<double>(ring("declination")));
				ringParam.normal = glm::vec3(
					-sin(rightAscension)*cos(declination),
					 cos(rightAscension)*cos(declination),
					 sin(declination));
				assetPaths.backscatFilename = get<std::string>(ring("backscat"));
				assetPaths.forwardscatFilename = get<std::string>(ring("forwardscat"));
				assetPaths.unlitFilename = get<std::string>(ring("unlit"));
				assetPaths.transparencyFilename = get<std::string>(ring("transparency"));
				assetPaths.colorFilename = get<std::string>(ring("color"));
			}
			planetParams[i] = planet;
		}
		// Assign planet parents
		for (uint32_t i=0;i<planetCount;++i)
		{
			const std::string parent = planetParams[i].parentName;
			if (parent != "")
			{
				for (uint32_t j=0;j<planetCount;++j)
				{
					if (i==j) continue;
					if (planetParams[j].name == parent)
					{
						planetParents[i] = j;
						break;
					}
				}
			}
		}	
	} 
	catch (parse_error &e)
	{
		std::cout << e << std::endl;
	}
}

bool Game::isPressedOnce(const int key)
{
	if (glfwGetKey(win, key))
	{
		if (keysHeld[key]) return false;
		else return (keysHeld[key] = true);
	}
	else
	{
		return (keysHeld[key] = false);
	}
}

void Game::update(const double dt)
{
	epoch += timeWarpValues[timeWarpIndex]*dt;

	std::vector<glm::dvec3> relativePositions(planetCount);
	// Planet state update
	for (uint32_t i=0;i<planetCount;++i)
	{
		// Relative position update
		if (planetParents[i] == -1)
			planetStates[i].position = glm::dvec3(0,0,0);
		else
		{
			relativePositions[i] = 
				planetParams[i].orbitParam.computePosition(
					epoch, planetParams[planetParents[i]].bodyParam.GM);
		}
		// Rotation
		planetStates[i].rotationAngle = (2.0*glm::pi<float>()*epoch)/planetParams[i].bodyParam.rotationPeriod + glm::pi<float>();
		// Cloud rotation
		const float period = planetParams[i].bodyParam.cloudDispPeriod;
		planetStates[i].cloudDisp = (period)?
			fmod(-epoch/period, 1.f):
			0.f;
	}

	// Planet absolute position update
	for (uint32_t i=0;i<planetCount;++i)
	{
		planetStates[i].position = relativePositions[i];
		int parent = planetParents[i];
		while (parent != -1)
		{
			planetStates[i].position += relativePositions[parent];
			parent = planetParents[parent];
		}
	}

	// Time warping
	if (isPressedOnce(GLFW_KEY_K))
	{
		if (timeWarpIndex > 0) timeWarpIndex--;
	}
	if (isPressedOnce(GLFW_KEY_L))
	{
		if (timeWarpIndex < (int)timeWarpValues.size()-1) timeWarpIndex++;
	}

	// Switching
	if (isPressedOnce(GLFW_KEY_TAB))
	{
		if (isSwitching)
		{
			isSwitching = false;
			cameraPolar.z = planetParams[focusedPlanetId].bodyParam.radius*4;
		}
		else
		{
			switchPreviousPlanet = focusedPlanetId;
			focusedPlanetId = (focusedPlanetId+1)%planetCount;
			switchPreviousDist = cameraPolar.z;
			isSwitching = true;
		} 
	}

	if (isSwitching)
	{
		const float t = switchFrameCurrent/(float)switchFrames;
		const double f = 6*t*t*t*t*t-15*t*t*t*t+10*t*t*t;
		const glm::dvec3 previousPlanetPos = planetStates[switchPreviousPlanet].position;
		cameraCenter = (planetStates[focusedPlanetId].position - previousPlanetPos)*f + previousPlanetPos;
		const float targetDist = planetParams[focusedPlanetId].bodyParam.radius*4;
		cameraPolar.z = (targetDist - switchPreviousDist)*f + switchPreviousDist;

		++switchFrameCurrent;
	}
	else
	{
		cameraCenter = planetStates[focusedPlanetId].position;
	}

	if (switchFrameCurrent > switchFrames)
	{
		isSwitching = false;
		switchFrameCurrent = 0;
	}

	// Mouse move
	double posX, posY;
	glfwGetCursorPos(win, &posX, &posY);
	const glm::vec2 move = {-posX+preMousePosX, posY-preMousePosY};

	bool mouseButton1 = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_1);
	bool mouseButton2 = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_2);

	// Check if we are not clicking on a gui
	if ((mouseButton1 || mouseButton2) && !dragging)
	{
		dragging = true;
	}
	else if (dragging && !(mouseButton1 || mouseButton2))
	{
		dragging = false;
	}

	if (dragging)
	{
		if (mouseButton1)
		{	
			viewSpeed.x += move.x*sensitivity;
			viewSpeed.y += move.y*sensitivity;
			for (int i=0;i<2;++i)
			{
				if (viewSpeed[i] > maxViewSpeed) viewSpeed[i] = maxViewSpeed;
				if (viewSpeed[i] < -maxViewSpeed) viewSpeed[i] = -maxViewSpeed;
			}
		}
		else if (mouseButton2)
		{
			viewSpeed.z += (move.y*sensitivity);
		}
	}

	cameraPolar.x += viewSpeed.x;
	cameraPolar.y += viewSpeed.y;
	cameraPolar.z *= 1.0+viewSpeed.z;

	viewSpeed *= viewSmoothness;

	if (cameraPolar.y > glm::pi<float>()/2 - 0.001)
	{
		cameraPolar.y = glm::pi<float>()/2 - 0.001;
		viewSpeed.y = 0;
	}
	if (cameraPolar.y < -glm::pi<float>()/2 + 0.001)
	{
		cameraPolar.y = -glm::pi<float>()/2 + 0.001;
		viewSpeed.y = 0;
	}
	const float radius = planetParams[focusedPlanetId].bodyParam.radius;
	if (cameraPolar.z < radius) cameraPolar.z = radius;

	// Mouse reset
	preMousePosX = posX;
	preMousePosY = posY;

	if (isPressedOnce(GLFW_KEY_F12))
	{
		renderer->takeScreenshot(generateScreenshotName());
	}

	// Shift scene so view is at (0,0,0)
	cameraPos = glm::dvec3(
		cos(cameraPolar.x)*cos(cameraPolar.y), 
		sin(cameraPolar.x)*cos(cameraPolar.y), 
		sin(cameraPolar.y))*(double)cameraPolar.z +
		cameraCenter;
		
	// Scene rendering
	renderer->render(
		cameraPos, glm::radians(CAMERA_FOVY), cameraCenter, glm::vec3(0,0,1),
		exposure, ambientColor,
		planetStates);

	auto a = renderer->getProfilerTimes();

	// Display profiler in console
	if (isPressedOnce(GLFW_KEY_F5) && !a.empty())
	{
		uint64_t full = a[0].second;
		int largestName = 0;
		for (auto p : a)
		{
			if (p.first.size() > largestName) largestName = p.first.size();
		}
		for (auto p : a)
		{
			std::cout.width(largestName);
			std::cout << std::left << p.first;
			uint64_t nano = (double)p.second;
			double percent = 100*nano/(double)full;
			double micro = nano/1E6;
			std::cout << "  " << micro << "ms (" << percent << "%)" << std::endl; 
		}
		std::cout << "-------------------------" << std::endl;
	}

	glfwSwapBuffers(win);
	glfwPollEvents();
}

bool Game::isRunning()
{
	return !glfwGetKey(win, GLFW_KEY_ESCAPE) && !glfwWindowShouldClose(win);
}

std::string generateScreenshotName()
{
	time_t t = time(0);
	struct tm *now = localtime(&t);
	std::stringstream filenameBuilder;
	filenameBuilder << 
		"./screenshots/screenshot_" << 
		(now->tm_year+1900) << "-" << 
		(now->tm_mon+1) << "-" << 
		(now->tm_mday) << "_" << 
		(now->tm_hour) << "-" << 
		(now->tm_min) << "-" << 
		(now->tm_sec) << ".png";
	return filenameBuilder.str();
}