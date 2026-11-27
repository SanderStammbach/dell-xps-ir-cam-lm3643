// SPDX-License-Identifier: GPL-2.0-only
// ir-bridge: libcamera IR sensor (raw R10 mono) -> v4l2loopback (GREY/YUYV)
//
// The HM1092 IR sensor on IPU6 laptops only exposes an unprocessed 10-bit
// mono stream (R10). libcamera's soft-ISP and GStreamer can't convert it, so
// applications like Howdy (OpenCV/V4L2) can't use it directly. This program
// captures R10 frames, does a simple auto-exposure via the sensor subdev
// controls, converts to 8 bit and writes them to a v4l2loopback device.

#include <libcamera/libcamera.h>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <getopt.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace libcamera;
using Clock = std::chrono::steady_clock;

static std::atomic<bool> g_stop{false};
static void onSignal(int) { g_stop = true; }

struct Options {
	std::string camera;            // libcamera id; empty = first camera whose model matches
	std::string model = "hm1092";
	std::string device = "/dev/video43";
	bool yuyv = false;
	double timeout = 0;            // seconds, 0 = run forever
	int exposure = -1;             // fixed sensor exposure (lines); -1 = auto
	int gain = -1;                 // fixed analogue gain; -1 = auto
	int target = 110;              // AE target mean (8 bit)
	int black = 32;                // black level (10 bit)
	bool verbose = false;
	int led = -1;                  // IR flood LED brightness; -1 = LED maximum, 0 = off
};

static void usage(const char *argv0)
{
	std::printf(
		"Usage: %s [options]\n"
		"  -c, --camera ID      libcamera camera id (default: first with model --model)\n"
		"  -m, --model NAME     sensor model to look for (default hm1092)\n"
		"  -d, --device PATH    v4l2loopback output device (default /dev/video43)\n"
		"  -y, --yuyv           output YUYV instead of GREY\n"
		"  -t, --timeout SEC    stop after SEC seconds (default: run until SIGINT/SIGTERM)\n"
		"  -e, --exposure N     fixed sensor exposure in lines (disables auto exposure)\n"
		"  -g, --gain N         fixed analogue gain 0-255 (disables auto exposure)\n"
		"  -T, --target N       auto-exposure target mean, 8 bit (default 110)\n"
		"  -b, --black N        sensor black level, 10 bit (default 32)\n"
		"  -v, --verbose        print brightness/exposure once per second\n"
		"  -L, --led N          IR flood LED (lm3643-ir) brightness, 0 = off (default: LED maximum)\n",
		argv0);
}

// ---- sensor subdev controls (exposure / gain) -------------------------------

class SensorCtl {
public:
	bool open(const std::string &model)
	{
		DIR *d = opendir("/sys/class/video4linux");
		if (!d)
			return false;
		while (dirent *e = readdir(d)) {
			if (std::strncmp(e->d_name, "v4l-subdev", 10))
				continue;
			std::ifstream f(std::string("/sys/class/video4linux/") + e->d_name + "/name");
			std::string name;
			std::getline(f, name);
			if (name.rfind(model, 0) == 0) {
				path_ = std::string("/dev/") + e->d_name;
				fd_ = ::open(path_.c_str(), O_RDWR);
				break;
			}
		}
		closedir(d);
		return fd_ >= 0;
	}
	~SensorCtl() { if (fd_ >= 0) ::close(fd_); }

	bool range(uint32_t id, int &min, int &max)
	{
		v4l2_queryctrl q{};
		q.id = id;
		if (ioctl(fd_, VIDIOC_QUERYCTRL, &q) < 0)
			return false;
		min = q.minimum;
		max = q.maximum;
		return true;
	}
	bool set(uint32_t id, int v)
	{
		v4l2_control c{id, v};
		return ioctl(fd_, VIDIOC_S_CTRL, &c) == 0;
	}
	bool get(uint32_t id, int &v)
	{
		v4l2_control c{id, 0};
		if (ioctl(fd_, VIDIOC_G_CTRL, &c) < 0)
			return false;
		v = c.value;
		return true;
	}
	const std::string &path() const { return path_; }
	bool ok() const { return fd_ >= 0; }

private:
	int fd_ = -1;
	std::string path_;
};

// ---- IR flood LED (lm3643-ir kernel module, LED class) ---------------------

class IrLed {
public:
	static constexpr const char *dir = "/sys/class/leds/lm3643:ir:flood/";
	explicit IrLed(int level)
	{
		if (level == 0)
			return;
		if (level < 0) {
			std::ifstream m(std::string(dir) + "max_brightness");
			if (!(m >> level))
				return;   // no LED driver loaded
		}
		fd_ = ::open((std::string(dir) + "brightness").c_str(), O_WRONLY);
		if (fd_ < 0 || !write(level)) {
			std::fprintf(stderr, "warning: cannot switch IR LED: %s\n", std::strerror(errno));
			return;
		}
		std::fprintf(stderr, "IR LED on (brightness %d)\n", level);
	}
	~IrLed()
	{
		if (fd_ >= 0) {
			write(0);
			::close(fd_);
		}
	}

private:
	bool write(int v)
	{
		const std::string s = std::to_string(v) + "\n";
		return ::pwrite(fd_, s.data(), s.size(), 0) == ssize_t(s.size());
	}
	int fd_ = -1;
};

// ---- main -------------------------------------------------------------------

int main(int argc, char **argv)
{
	Options o;
	static const option longOpts[] = {
		{"camera", required_argument, nullptr, 'c'},
		{"model", required_argument, nullptr, 'm'},
		{"device", required_argument, nullptr, 'd'},
		{"yuyv", no_argument, nullptr, 'y'},
		{"timeout", required_argument, nullptr, 't'},
		{"exposure", required_argument, nullptr, 'e'},
		{"gain", required_argument, nullptr, 'g'},
		{"target", required_argument, nullptr, 'T'},
		{"black", required_argument, nullptr, 'b'},
		{"verbose", no_argument, nullptr, 'v'},
		{"led", required_argument, nullptr, 'L'},
		{"help", no_argument, nullptr, 'h'},
		{},
	};
	for (int c; (c = getopt_long(argc, argv, "c:m:d:yt:e:g:T:b:vL:h", longOpts, nullptr)) != -1;) {
		switch (c) {
		case 'c': o.camera = optarg; break;
		case 'm': o.model = optarg; break;
		case 'd': o.device = optarg; break;
		case 'y': o.yuyv = true; break;
		case 't': o.timeout = std::atof(optarg); break;
		case 'e': o.exposure = std::atoi(optarg); break;
		case 'g': o.gain = std::atoi(optarg); break;
		case 'T': o.target = std::atoi(optarg); break;
		case 'b': o.black = std::atoi(optarg); break;
		case 'v': o.verbose = true; break;
		case 'L': o.led = std::atoi(optarg); break;
		default: usage(argv[0]); return c == 'h' ? 0 : 1;
		}
	}

	std::signal(SIGINT, onSignal);
	std::signal(SIGTERM, onSignal);

	// -- camera
	auto cm = std::make_unique<CameraManager>();
	if (cm->start()) {
		std::fprintf(stderr, "failed to start camera manager\n");
		return 1;
	}
	std::shared_ptr<Camera> cam;
	if (!o.camera.empty()) {
		cam = cm->get(o.camera);
	} else {
		for (auto &c : cm->cameras()) {
			auto model = c->properties().get(properties::Model);
			if (model && model->rfind(o.model, 0) == 0) {
				cam = c;
				break;
			}
		}
	}
	if (!cam) {
		std::fprintf(stderr, "camera not found\n");
		return 1;
	}
	if (cam->acquire()) {
		std::fprintf(stderr, "failed to acquire camera %s (in use?)\n", cam->id().c_str());
		return 1;
	}

	auto config = cam->generateConfiguration({ StreamRole::Raw });
	if (!config)
		config = cam->generateConfiguration({ StreamRole::Viewfinder });
	StreamConfiguration &sc = config->at(0);
	sc.pixelFormat = formats::R10;
	sc.bufferCount = 4;
	if (config->validate() == CameraConfiguration::Invalid || sc.pixelFormat != formats::R10) {
		std::fprintf(stderr, "camera does not offer unpacked R10 (got %s)\n",
			     sc.pixelFormat.toString().c_str());
		return 1;
	}
	if (cam->configure(config.get())) {
		std::fprintf(stderr, "failed to configure camera\n");
		return 1;
	}
	const unsigned W = sc.size.width, H = sc.size.height, stride = sc.stride;
	Stream *stream = sc.stream();
	std::fprintf(stderr, "camera %s: %ux%u R10 stride %u\n", cam->id().c_str(), W, H, stride);

	FrameBufferAllocator alloc(cam);
	if (alloc.allocate(stream) < 0) {
		std::fprintf(stderr, "buffer allocation failed\n");
		return 1;
	}

	// map buffers (all planes of a buffer share one dmabuf; map it once)
	std::map<const FrameBuffer *, const uint8_t *> mapped;
	std::vector<std::pair<void *, size_t>> maps;
	std::vector<std::unique_ptr<Request>> requests;
	for (const auto &buf : alloc.buffers(stream)) {
		const auto &p = buf->planes()[0];
		size_t len = p.offset + p.length;
		void *m = mmap(nullptr, len, PROT_READ, MAP_SHARED, p.fd.get(), 0);
		if (m == MAP_FAILED) {
			std::perror("mmap");
			return 1;
		}
		maps.emplace_back(m, len);
		mapped[buf.get()] = static_cast<const uint8_t *>(m) + p.offset;

		auto req = cam->createRequest();
		if (!req || req->addBuffer(stream, buf.get())) {
			std::fprintf(stderr, "failed to create request\n");
			return 1;
		}
		requests.push_back(std::move(req));
	}

	// -- loopback output
	int out = ::open(o.device.c_str(), O_RDWR);
	if (out < 0) {
		std::fprintf(stderr, "open %s: %s\n", o.device.c_str(), std::strerror(errno));
		return 1;
	}
	const unsigned bpp = o.yuyv ? 2 : 1;
	v4l2_format fmt{};
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = W;
	fmt.fmt.pix.height = H;
	fmt.fmt.pix.pixelformat = o.yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_GREY;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = W * bpp;
	fmt.fmt.pix.sizeimage = W * H * bpp;
	fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	if (ioctl(out, VIDIOC_S_FMT, &fmt) < 0) {
		std::fprintf(stderr, "VIDIOC_S_FMT on %s: %s (is it a v4l2loopback device?)\n",
			     o.device.c_str(), std::strerror(errno));
		return 1;
	}
	std::vector<uint8_t> frame(W * H * bpp);

	// -- sensor controls / auto exposure
	SensorCtl sensor;
	int expMin = 0, expMax = 0, gainMin = 0, gainMax = 0;
	bool haveExp = false, haveGain = false;
	if (sensor.open(o.model)) {
		haveExp = sensor.range(V4L2_CID_EXPOSURE, expMin, expMax);
		haveGain = sensor.range(V4L2_CID_ANALOGUE_GAIN, gainMin, gainMax);
	} else {
		std::fprintf(stderr, "warning: sensor subdev for '%s' not found, no exposure control\n",
			     o.model.c_str());
	}
	const bool autoExp = o.exposure < 0 && o.gain < 0 && haveExp && haveGain;
	int exposure = o.exposure >= 0 ? o.exposure : expMax;   // start long, gain low
	int gain = o.gain >= 0 ? o.gain : gainMin;
	auto applySensor = [&] {
		if (haveExp)
			sensor.set(V4L2_CID_EXPOSURE, std::clamp(exposure, expMin, expMax));
		if (haveGain)
			sensor.set(V4L2_CID_ANALOGUE_GAIN, std::clamp(gain, gainMin, gainMax));
	};

	// -- capture loop: completed requests are handed to the main thread
	std::mutex mtx;
	std::condition_variable cv;
	std::deque<Request *> done;
	cam->requestCompleted.connect(cam.get(), [&](Request *r) {
		std::lock_guard<std::mutex> l(mtx);
		done.push_back(r);
		cv.notify_one();
	});

	if (cam->start()) {
		std::fprintf(stderr, "failed to start camera\n");
		return 1;
	}
	applySensor();
	IrLed led(o.led);   // switched off again when main() returns
	for (auto &r : requests)
		cam->queueRequest(r.get());

	const auto t0 = Clock::now();
	auto tLog = t0;
	unsigned frames = 0, settle = 0;
	const int black = std::clamp(o.black, 0, 1000);
	const float scale = 255.0f / (1023 - black);

	while (!g_stop) {
		if (o.timeout > 0 &&
		    std::chrono::duration<double>(Clock::now() - t0).count() > o.timeout)
			break;

		Request *req;
		{
			std::unique_lock<std::mutex> l(mtx);
			if (!cv.wait_for(l, std::chrono::milliseconds(500), [&] { return !done.empty(); }))
				continue;
			req = done.front();
			done.pop_front();
		}
		if (req->status() == Request::RequestCancelled)
			break;

		const FrameBuffer *fb = req->buffers().begin()->second;
		const uint8_t *src = mapped[fb];
		uint64_t sum = 0;
		for (unsigned y = 0; y < H; y++) {
			const uint16_t *line = reinterpret_cast<const uint16_t *>(src + y * stride);
			uint8_t *dst = frame.data() + y * W * bpp;
			for (unsigned x = 0; x < W; x++) {
				int v = int(line[x] & 0x3ff) - black;
				int p = v <= 0 ? 0 : std::min(255, int(v * scale + 0.5f));
				sum += p;
				if (bpp == 1) {
					dst[x] = p;
				} else {
					dst[2 * x] = p;
					dst[2 * x + 1] = 128;
				}
			}
		}
		req->reuse(Request::ReuseBuffers);
		cam->queueRequest(req);

		if (::write(out, frame.data(), frame.size()) < 0 && errno != EAGAIN) {
			std::fprintf(stderr, "write %s: %s\n", o.device.c_str(), std::strerror(errno));
			break;
		}
		frames++;

		// simple AE: the sensor applies new values with a few frames delay,
		// so only adjust every 4th frame. Prefer long exposure, then gain.
		const double mean = double(sum) / (W * H);
		if (autoExp && ++settle >= 4) {
			settle = 0;
			const double err = o.target / std::max(mean, 1.0);
			if (err > 1.15) {
				if (exposure < expMax)
					exposure = std::min(expMax, int(exposure * std::min(err, 2.0)) + 1);
				else
					gain = std::min(gainMax, gain + std::max(1, gain / 6));
				applySensor();
			} else if (err < 0.87) {
				if (gain > gainMin)
					gain = std::max(gainMin, gain - std::max(1, gain / 6));
				else
					exposure = std::max(expMin, int(exposure * std::max(err, 0.5)));
				applySensor();
			}
		}

		if (o.verbose && Clock::now() - tLog > std::chrono::seconds(1)) {
			tLog = Clock::now();
			std::fprintf(stderr, "frames %u  mean %.1f  exposure %d  gain %d\n",
				     frames, mean, exposure, gain);
		}
	}

	cam->stop();
	cam->requestCompleted.disconnect(cam.get());
	requests.clear();
	for (auto &[m, len] : maps)
		munmap(m, len);
	alloc.free(stream);
	cam->release();
	cam.reset();
	cm->stop();
	::close(out);

	// leave the sensor in its default state for other users
	if (sensor.ok()) {
		exposure = (expMin + expMax) / 2;  // driver default is 360 of 2..719
		gain = gainMin;
		applySensor();
	}
	std::fprintf(stderr, "stopped after %u frames\n", frames);
	return 0;
}
