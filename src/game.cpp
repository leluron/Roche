#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <iostream>

#include "game.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <fstream>

#include <nanogui/screen.h>
#include <nanogui/window.h>
#include <nanogui/widget.h>
#include <nanogui/label.h>
#include <nanogui/slider.h>
#include <nanogui/layout.h>
#include <nanogui/textbox.h>

#include "renderer_gl.hpp"

#include "thirdparty/shaun/sweeper.hpp"
#include "thirdparty/shaun/parser.hpp"
#include "thirdparty/lodepng.h"

#include <glm/ext.hpp>

// Ridiculous hack so glfw callbacks can take a lambda
nanogui::Screen *screenPtr;

const float CAMERA_FOVY = 40.0;

Game::Game()
{
	sensitivity = 0.0004;
	viewSpeed = glm::vec3(0,0,0);
	maxViewSpeed = 0.2;
	viewSmoothness = 0.85;
	switchPreviousPlanet = -1;
	save = false;

	timeWarpValues = {1, 60, 60*10, 3600, 3600*3, 3600*12, 3600*24, 3600*24*10, 3600*24*365.2499};
	timeWarpIndex = 0;

	switchFrames = 100;
	switchFrameCurrent = 0;
	isSwitching = false;
	focusedPlanetId = 0;
	epoch = 0.0;
	quit = false;

	msaaSamples = 1;
	ssaa = 0.0;
	gamma = 2.2;
	exposure = 0;

	renderer.reset(new RendererGL());
}

Game::~Game()
{
	renderer->destroy();

	glfwTerminate();

	quit = true;
	screenshotThread.join();
}

void ssThread(
	const std::atomic<bool> &quit, std::vector<uint8_t> &buffer,
	std::atomic<bool> &save, const int width, const int height)
{
	while (!quit)
	{
		if (save)
		{
			time_t t = time(0);
			struct tm *now = localtime(&t);
			std::stringstream filename;
			filename << 
				"./screenshots/screenshot_" << 
				(now->tm_year+1900) << "-" << 
				(now->tm_mon+1) << "-" << 
				(now->tm_mday) << "_" << 
				(now->tm_hour) << "-" << 
				(now->tm_min) << "-" << 
				(now->tm_sec) << ".png";

			unsigned int error = lodepng::encode(filename.str(), buffer.data(), width, height);
			if (error)
			{
				std::cout << "Can't save screenshot: " << lodepng_error_text(error) << std::endl;
			}
			save = false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

std::string readFile(const std::string filename)
{
	std::ifstream in(filename.c_str(), std::ios::in | std::ios::binary);
	if (in)
	{
		std::string contents;
		in.seekg(0, std::ios::end);
		contents.resize(in.tellg());
		in.seekg(0, std::ios::beg);
		in.read(&contents[0], contents.size());
		in.close();
		return(contents);
	}
	throw std::runtime_error("Can't open" + filename);
	return "";
}

void Game::loadSettingsFile()
{
	using namespace shaun;
	try 
	{
		parser p;
		std::string fileContent = readFile("config/settings.sn");
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
		DDSLoader::setSkipMipmap(graphics("skipMipmap").value<number>());
		msaaSamples = graphics("msaaSamples").value<number>();
		ssaa = graphics("ssaa").value<boolean>();
		gamma = graphics("gamma").value<number>();

		sweeper controls(swp("controls"));
		sensitivity = controls("sensitivity").value<number>();
	} 
	catch (parse_error e)
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
	glfwWindowHint(GLFW_DEPTH_BITS, 0);
	glfwWindowHint(GLFW_STENCIL_BITS, 0);
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

	const GLenum err = glewInit();
	if (err != GLEW_OK)
	{
		throw std::runtime_error("Can't initialize GLEW : " + std::string((const char*)glewGetErrorString(err)));
	}

	// Screenshot
	screenshotBuffer.resize(width*height*4);
	screenshotThread = std::thread(
		ssThread, std::ref(quit), std::ref(screenshotBuffer), std::ref(save), width, height);

	renderer->init(planetParams, msaaSamples, ssaa, width, height);
	initGUI();
}

class ImmovableWindow : public nanogui::Window
{
public:
	ImmovableWindow(Widget *parent, const std::string &title) : Window(parent, title) {}
	bool mouseDragEvent(const Eigen::Vector2i &, const Eigen::Vector2i &, int, int)
	{
		return false;
	}
	bool mouseButtonEvent(const Eigen::Vector2i &p, int button, bool down, int modifiers)
	{
		if (Widget::mouseButtonEvent(p, button, down, modifiers)) return true;
		return false;
	}
};

void Game::createSettingsWindow()
{
	using namespace nanogui;
	using namespace Eigen;

	Widget *panel;
	Slider *slider;
	TextBox *textBox;

	// Settings window
	ImmovableWindow *window = new ImmovableWindow(guiScreen, "Settings");
	window->setPosition(Vector2i(10, 10));
	window->setLayout(new GroupLayout());

	// Gamma slider
	new Label(window, "Gamma");
	panel = new Widget(window);
	panel->setLayout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 20));

	slider = new Slider(panel);
	slider->setValue(gamma);
	slider->setFixedWidth(140);
	slider->setRange({1.8, 2.4});

	textBox = new TextBox(panel);
	textBox->setFixedSize(Vector2i(60, 25));
	textBox->setFontSize(20);
	textBox->setAlignment(TextBox::Alignment::Right);

	auto showGammaValue = [](float value) {
		std::stringstream stream;
		stream << std::fixed << std::setprecision(2) << value;
		return stream.str();
	};

	slider->setCallback([this,textBox,showGammaValue](float value) {
		textBox->setValue(showGammaValue(value));
		gamma = value;
	});

	textBox->setValue(showGammaValue(gamma));

	// Exposure slider
	new Label(window, "Exposure");
	panel = new Widget(window);
	panel->setLayout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 20));

	slider = new Slider(panel);
	slider->setValue(exposure);
	slider->setFixedWidth(140);
	slider->setRange({0, 6});

	textBox = new TextBox(panel);
	textBox->setFixedSize(Vector2i(60, 25));
	textBox->setFontSize(20);
	textBox->setAlignment(TextBox::Alignment::Right);

	auto showExpValue = [](float value) {
		std::stringstream stream;
		stream << std::fixed << std::setprecision(1) << value;
		return stream.str();
	};

	slider->setCallback([this,textBox,showExpValue](float value) {
		textBox->setValue(showExpValue(value));
		exposure = value;
	});

	textBox->setValue(showExpValue(exposure));
}

void Game::initGUI()
{
	using namespace nanogui;
	using namespace Eigen;

	guiScreen = new Screen();
	guiScreen->initialize(win, false);

	createSettingsWindow();

	guiScreen->setVisible(true);
	guiScreen->performLayout();
	
	// GUI callbacks
	screenPtr = guiScreen;
	glfwSetCursorPosCallback(win,
		[](GLFWwindow *, double x, double y) {
			screenPtr->cursorPosCallbackEvent(x, y);
		}
	);

	glfwSetMouseButtonCallback(win,
		[](GLFWwindow *, int button, int action, int modifiers) {
			screenPtr->mouseButtonCallbackEvent(button, action, modifiers);
		}
	);

	glfwSetKeyCallback(win,
		[](GLFWwindow *, int key, int scancode, int action, int mods) {
			screenPtr->keyCallbackEvent(key, scancode, action, mods);
		}
	);

	glfwSetCharCallback(win,
		[](GLFWwindow *, unsigned int codepoint) {
			screenPtr->charCallbackEvent(codepoint);
		}
	);

	glfwSetDropCallback(win,
		[](GLFWwindow *, int count, const char **filenames) {
			screenPtr->dropCallbackEvent(count, filenames);
		}
	);

	glfwSetScrollCallback(win,
		[](GLFWwindow *, double x, double y) {
			screenPtr->scrollCallbackEvent(x, y);
		}
	);

	glfwSetFramebufferSizeCallback(win,
		[](GLFWwindow *, int width, int height) {
			screenPtr->resizeCallbackEvent(width, height);
		}
	);
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
			PlanetParameters planet;
			sweeper pl(planetsSweeper[i]);
			planet.name = std::string(pl("name").value<string>());
			planet.parentName = get<std::string>(pl("parent"));

			sweeper orbit(pl("orbit"));
			if (!orbit.is_null())
			{
				planet.orbitParam.ecc = get<double>(orbit("ecc"));
				planet.orbitParam.sma = get<double>(orbit("sma"));
				planet.orbitParam.inc = glm::radians(get<double>(orbit("inc")));
				planet.orbitParam.lan = glm::radians(get<double>(orbit("lan")));
				planet.orbitParam.arg = glm::radians(get<double>(orbit("arg")));
				planet.orbitParam.m0  = glm::radians(get<double>(orbit("m0" )));
			}
			sweeper body(pl("body"));
			if (!body.is_null())
			{
				planet.bodyParam.radius = get<double>(body("radius"));
				float rightAscension = glm::radians(get<double>(body("rightAscension")));
				float declination = glm::radians(get<double>(body("declination")));
				planet.bodyParam.rotationAxis = glm::vec3(
					-sin(rightAscension)*cos(declination),
					 cos(rightAscension)*cos(declination),
					 sin(declination));
				planet.bodyParam.rotationPeriod = get<double>(body("rotPeriod"));
				planet.bodyParam.meanColor = get<glm::vec3>(body("meanColor"));
				planet.bodyParam.albedo = get<double>(body("albedo"));
				planet.bodyParam.GM = get<double>(body("GM"));
				planet.bodyParam.isStar = get<bool>(  body("isStar"));
				planet.assetPaths.diffuseFilename = get<std::string>(body("diffuse"));
				planet.assetPaths.nightFilename = get<std::string>(body("night"));
				planet.assetPaths.cloudFilename = get<std::string>(body("cloud"));
				planet.assetPaths.modelFilename = get<std::string>(body("model"));
				planet.bodyParam.cloudDispPeriod = get<double>(body("cloudDispPeriod"));
				planet.bodyParam.nightTexIntensity = get<double>(body("nightTexIntensity"));
			}
			sweeper atmo(pl("atmosphere"));
			planet.atmoParam.hasAtmosphere = !atmo.is_null();
			if (!atmo.is_null())
			{
				planet.atmoParam.hasAtmosphere = true;
				planet.atmoParam.maxHeight = get<double>(atmo("maxAltitude"));
				planet.atmoParam.K = get<glm::vec4>(atmo("K"));
				planet.atmoParam.density = get<double>(atmo("density"));
				planet.atmoParam.scaleHeight = get<double>(atmo("scaleHeight"));
			}

			sweeper ring(pl("ring"));
			planet.ringParam.hasRings = !ring.is_null();
			if (!ring.is_null())
			{
				planet.ringParam.innerDistance = get<double>(ring("inner"));
				planet.ringParam.outerDistance = get<double>(ring("outer"));
				float rightAscension = glm::radians(get<double>(ring("rightAscension")));
				float declination = glm::radians(get<double>(ring("declination")));
				planet.ringParam.normal = glm::vec3(
					-sin(rightAscension)*cos(declination),
					 cos(rightAscension)*cos(declination),
					 sin(declination));
				planet.ringParam.seed = (int)get<double>(ring("seed"));
				planet.ringParam.color = get<glm::vec4>(ring("color"));
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
	catch (parse_error e)
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
		planetStates[i].rotationAngle = (2.0*PI*epoch)/planetParams[i].bodyParam.rotationPeriod + PI;
		// Cloud rotation
		const float period = planetParams[i].bodyParam.cloudDispPeriod;
		planetStates[i].cloudDisp = (period)?
			std::fmod(-epoch/period, 1.f):
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
		nanogui::Widget *w = guiScreen->findWidget(Eigen::Vector2i(posX, posY));
		canDrag = (!w || w == guiScreen);
	}
	else if (dragging && !(mouseButton1 || mouseButton2))
	{
		dragging = false;
	}

	if (dragging && canDrag)
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
		else if (glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_2))
		{
			viewSpeed.z += (move.y*sensitivity);
		}
	}

	cameraPolar.x += viewSpeed.x;
	cameraPolar.y += viewSpeed.y;
	cameraPolar.z *= 1.0+viewSpeed.z;

	viewSpeed *= viewSmoothness;

	if (cameraPolar.y > PI/2 - 0.001)
	{
		cameraPolar.y = PI/2 - 0.001;
		viewSpeed.y = 0;
	}
	if (cameraPolar.y < -PI/2 + 0.001)
	{
		cameraPolar.y = -PI/2 + 0.001;
		viewSpeed.y = 0;
	}
	const float radius = planetParams[focusedPlanetId].bodyParam.radius;
	if (cameraPolar.z < radius) cameraPolar.z = radius;

	// Mouse reset
	preMousePosX = posX;
	preMousePosY = posY;

	if (isPressedOnce(GLFW_KEY_F12) && !save)
	{
		int width,height;
		glfwGetWindowSize(win, &width, &height);
		glReadPixels(0,0,width,height, GL_RGBA, GL_UNSIGNED_BYTE, screenshotBuffer.data());
		save = true;
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
		gamma, exposure, ambientColor,
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

	// GUI rendering
	guiScreen->drawWidgets();

	glfwSwapBuffers(win);
	glfwPollEvents();
}

bool Game::isRunning()
{
	return !glfwGetKey(win, GLFW_KEY_ESCAPE) && !glfwWindowShouldClose(win);
}