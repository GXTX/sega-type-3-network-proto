#include "sockpp/tcp_connector.h"
#include <any>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

#include <future>

#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

// https://github.com/aeldidi/crc32
extern "C" uint32_t crc32(const void* input, size_t size);

class NetDimmPacket {
public:
	uint8_t id                = 0;
	uint8_t flags             = 0; // Packet dependant flags
	uint16_t length           = 0;
	std::vector<uint8_t> data = {};

	static constexpr auto headerSize = 4;

	NetDimmPacket() {}

	template <typename T>
	NetDimmPacket(T ids, uint8_t flags, const std::vector<uint8_t>& buffer = {})
	        : flags(flags)
	{
		id   = static_cast<uint8_t>(ids);
		data = buffer;

		length = static_cast<uint16_t>(data.size());
		data.insert(data.begin(), id);
		data.insert(data.begin(), flags);
		data.insert(data.begin(), (length >> 8) & 0xFF);
		data.insert(data.begin(), length & 0xFF);
	}

	// TODO: Check data size before touching buffer
	NetDimmPacket(std::vector<uint8_t>& buffer)
	{
		id     = *(reinterpret_cast<uint8_t*>(&buffer[3]));
		flags  = *(reinterpret_cast<uint8_t*>(&buffer[2]));
		length = *(reinterpret_cast<uint16_t*>(&buffer[0]));

		if (buffer.size() > headerSize) {
			std::copy(buffer.begin() + headerSize,
			          buffer.end(),
			          std::back_inserter(data));
		}
	}

	void AppendData(const std::vector<uint8_t>& buffer)
	{
		std::copy(buffer.begin(), buffer.end(), std::back_inserter(data));
	}
};

class NetDimm {
public:
	// Items marked "." are NOP on Naomi 3.17, and "*" for Chihiro 13.05
	enum class Command : uint8_t {
		NOP                   = 0x00, // .*
		START_UP              = 0x01, // .*
		HEALTH_CHECK          = 0x02, // .*
		LOST_PACKET           = 0x03, // .*
		UNK1                  = 0x04, // nfWriteDimm
		REQUEST_FOR_DIMM      = 0x05, //
		TRANSFER_CANCEL       = 0x06, // .*
		HOST_MODE             = 0x07, // .
		DIMM_MODE             = 0x08, // .
		TERMINATE             = 0x09, //
		HOST_RESTART          = 0x0A, //
		WRITE_TO_FLASH        = 0x0B, //
		WRITE_EEPROM          = 0x0C, // WRITE_NVRAM
		READ_EEPROM           = 0x0D, // READ_NVRAM
		PEEK_HOST             = 0x10, //
		POKE_HOST             = 0x11, //
		ENABLE_OFF_LINE       = 0x14, // .*
		DISABLE_OFF_LINE      = 0x15, // .*
		GET_COIN_INFORMATION  = 0x16, //
		SET_TIME_LIMIT        = 0x17, //
		GET_DIMM_INFORMATION  = 0x18, //
		SET_DIMM_INFORMATION  = 0x19, //
		NETFIRM_INFO          = 0x1E, //
		RESET_FIRMWARE        = 0x1F, //
		SECOND_BOOT_UPDATE    = 0x20, //
		MEDIA_FORMAT          = 0x21, // 1 uint32_t, appears to modify DIMM_MODE but only if DIP7 on Chihiro is set?, Spider sets this as 0x0004?
		UNK2                  = 0x22, // Writes to NAND boards for Triforce/Chihiro, unknown if this works for SystemSP
		SET_MEDIA_INFO        = 0x25, // 2 uint32_t's, only output from Chihiro "This media is DIMM."
		MEDIA_READ            = 0x26, // lrpMediaRead | Spider (SystemSP) only
		MEDIA_ERASE           = 0x27, // Spider (SystemSP) only, trasn_ex
		MEDIA_STATUS          = 0x28, // Spider (SystemSP) only, trasn_ex checks this after issuing MEDIA_FORMAT
		SECURITY_KEYCODE      = 0x7F, //
		PeekHost16            = 0xF0, //
		PokeHost16            = 0xF1, //
		ControlRead           = 0xF2, // I'm not sure what this is actually reading
	};

	struct NetDimmInformation {
		uint16_t unk;
		uint16_t firmwareVersion;
		uint16_t gameMemorySize;
		uint16_t dimmMemorySize;
		uint32_t gameCrc;
	};

	NetDimm(const std::string_view& host,
	        std::optional<bool> keepAlive               = std::nullopt,
	        std::optional<std::chrono::seconds> timeout = std::nullopt)
	        : address(host)
	{
		// this->keepAlive = keepAlive.value_or(false);
		constexpr auto readTimeout = std::chrono::seconds(5);
		this->timeout              = timeout.value_or(readTimeout);
	}

	~NetDimm()
	{
		disconnect();
	}

	bool connect()
	{
		sockpp::initialize();

		uint32_t inet_addr = 0;
		// FIXME: this->address
		inet_pton(AF_INET, "4.0.0.10", &inet_addr);
		auto addr = sockpp::inet_address(inet_addr, port);

		if (!!connection.connect(addr)) {
			connection.nodelay(TRUE);
			connection.reuse_address(TRUE);
			connection.read_timeout(timeout);
			if (keepAlive) {
				std::thread([this] { KeepAliveThread(); }).detach();
			}
			// sendPacket({ Command::START_UP, 0 }); // Ends up
			// being a NOP on Chihiro
			return true;
		}

		return false;
	}

	bool disconnect()
	{
		// TODO: Check Naomi to make sure we aren't killing the network by issuing this, DragonMinded seems to think so
		sendPacket({Command::TERMINATE, 0});
		return !!connection.close();
	}

	void sendPacket(const NetDimmPacket& packet,
	                std::optional<sockpp::socket*> con = std::nullopt,
	                std::optional<bool> block          = std::nullopt)
	{
		auto sock = con.has_value() ? con.value() : &connection;
		// sock->set_non_blocking(block.value_or(true));
		sock->send(packet.data.data(), packet.data.size());
	}

	auto recvPacket(std::optional<sockpp::socket*> con = std::nullopt)
	{
		const auto sock = con.has_value() ? con.value() : &connection;

		// We're going to abuse this poor vector...
		std::vector<uint8_t> buffer(NetDimmPacket::headerSize, 0);

		// First read in the header bytes
		if (sock->recv(&buffer[0], NetDimmPacket::headerSize).is_error()) {
			return NetDimmPacket();
		}

		auto packet = NetDimmPacket(buffer);

		// std::printf("I: %d L: %d", packet.id, packet.length);
		auto toRead = packet.length;

		buffer.clear();
		buffer.resize(packet.length);

		while (toRead) {
			const auto result = sock->recv(&buffer[packet.length - toRead], toRead);

			if (result) {
				toRead -= static_cast<uint16_t>(result.value());
			} else {
				return NetDimmPacket();
			}
		}

		packet.AppendData(buffer);
		return packet;
	}

	// -----
	void nNop(void) { sendPacket({Command::NOP, 0}); }

	void nRestartHost(void) { sendPacket({Command::HOST_RESTART, 0}); }
	void nRestartFirmware(void) { sendPacket({Command::RESET_FIRMWARE, 0}); }

	void nSetTimeLimit(uint8_t hours)
	{
		/*
		  if (value - 1 < 10)
		    value = value * 60000;
		  else
		    value = 60000;
		  nfWriteDimm(0xFFFEFFE8,&value,4);
		*/
		if (hours > 10) {
			hours = 10;
		}
		sendPacket({ Command::SET_TIME_LIMIT, 0, {hours, 0, 0, 0} });
	}

	// Chihiro 11.00 & 13.05 appear only to check this twice: Once on
	// start-up to initiate a "Coin" service if BIT(0) is set, and on the
	// "Network Test" screen where it will append the text "ETHER MODE" with
	// the current set value.
	void nSetDimmMode(const uint8_t mode, const uint8_t mask = 0) { sendPacket({ Command::DIMM_MODE, 0, {mode, mask} }); }
	auto nGetDimmMode()
	{
		sendPacket({Command::DIMM_MODE, 0});
		return recvPacket().data;
	}

	// Set this to 1 to trigger "Now loading..."
	void nSetHostMode(const uint8_t mode, const uint8_t mask = 0) { sendPacket({Command::HOST_MODE, 0, {mode, mask}}); }
	auto nGetHostMode()
	{
		sendPacket({Command::HOST_MODE, 0});
		return recvPacket().data;
	}

	void nSetOfflineMode(const bool enable = true) { sendPacket({enable ? Command::ENABLE_OFF_LINE : Command::DISABLE_OFF_LINE, 0}); }

	// TODO: Check jumper settings before attempting to nWriteEeprom
	// Eeprom located on the netboard NOT mediaboard
	auto nReadEeprom()
	{
		sendPacket({Command::READ_EEPROM, 0});
		return recvPacket().data;
	}
	void nWriteEeprom(const std::vector<uint8_t>& data)
	{
		constexpr auto requiredWriteSize = 0x60;
		if (data.size() != requiredWriteSize) {
			return;
		}

		sendPacket({Command::WRITE_EEPROM, 0, data});
	}

	auto nReadCoinInfo()
	{
		sendPacket({Command::GET_COIN_INFORMATION, 0});
		return recvPacket().data;
	}

	auto nGetDimmInfo()
	{
		sendPacket({Command::GET_DIMM_INFORMATION, 0});
		const auto packet = recvPacket().data;
		NetDimmInformation info = {};
		std::memcpy(&info, packet.data(), sizeof(NetDimmInformation));
		return info;
	}
	void nSetDimmInfo(const uint32_t crc, const uint32_t length)
	{
		std::vector<uint8_t> info(8, 0);
		*(reinterpret_cast<uint32_t*>(&info[0])) = crc;
		*(reinterpret_cast<uint32_t*>(&info[4])) = length;
		sendPacket({Command::SET_DIMM_INFORMATION, lastPacket, info});
	}

	auto nReadNetfirmInfo()
	{
		sendPacket({Command::NETFIRM_INFO, 0});
		return recvPacket().data;
	}

	void nSetKeyCode(const std::vector<uint8_t>& key)
	{
		// Key is written to address 0xFFFEFFF0, oddly, it expects 8
		// bytes but when writing the information it writes 16 bytes
		constexpr auto requiredWriteSize = 8;
		if (key.size() != requiredWriteSize) {
			return;
		}

		sendPacket({Command::SECURITY_KEYCODE, 0, key});
	}

	// Type-3 Specific: 13.05 expects 2 uint32_t's to be zero, 11.00 only
	// requires one
	void nWriteSecondBoot(const std::vector<uint8_t>& data)
	{
		nUpload(netMemMask, data);
		sendPacket({Command::SECOND_BOOT_UPDATE, 0, {0, 0, 0, 0}});
	}

	// Type-3 Specific: If the jumper to utilize the 1st half of the flash
	// is set OR if on Ver1100 there is NO checking, otherwise buffer CRC
	// must be 0xFFFFFFFF
	void nWriteNetFlash(const std::vector<uint8_t>& data)
	{
		nUpload(netMemMask, data);
		sendPacket({Command::WRITE_TO_FLASH, 0});
	}

	// Used to write to the NAND on games like Mario Kart GP. If the machine
	// doesn't have a NAND board present it'll just write to DIMM memory automatically
	void nWriteDimmNand(uint32_t addr, const std::vector<uint8_t>& data)
	{
		const auto size = data.size();

		// FIXME: Refactor to have sendPacket take a void* so we can
		// avoid needless copies
		std::vector<uint8_t> temp = {};
		temp.reserve(maxRequestSize * 2);

		uint32_t offset = 0;
		bool end = false;

		auto start = std::chrono::high_resolution_clock::now();
		auto now = std::chrono::high_resolution_clock::now();
		size_t sent = 0;

		constexpr auto headerSize = 8;
		constexpr auto maxPayloadSize = 0x400;

		while (offset < size) {
			temp.clear();
			temp.resize(headerSize);

			*(reinterpret_cast<uint32_t*>(&temp[0])) = addr + offset;
			*(reinterpret_cast<uint32_t*>(&temp[4])) = 0; // FIXME: Doesn't seem to change anything?

			if (offset + maxPayloadSize > size) {
				end = true;
				std::copy(data.begin() + offset, data.end(), std::back_inserter(temp));
			}
			else {
				std::copy(data.begin() + offset, data.begin() + offset + maxPayloadSize, std::back_inserter(temp));
			}

			NetDimmPacket x = { Command::UNK2,
							   0, // FIXME: Does this really not care if we don't set the markers?
							   temp };
			sendPacket(x);
			offset += maxPayloadSize;

			sent += temp.size();
			now = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> duration = now - start;
			if (duration.count() >= 1.0 || end) {
				double speed = sent * duration.count() / 1000 / 1000;
				std::printf("%.2f (%08X / %08X)\n", speed, offset, size);
				sent = 0;
				start = std::chrono::high_resolution_clock::now();
			}
		}
	}

	void nUpload(uint32_t addr, const std::vector<uint8_t>& data)
	{
		const auto size = data.size();

		// FIXME: Refactor to have sendPacket take a void* so we can
		// avoid needless copies
		std::vector<uint8_t> temp = {};
		temp.reserve(maxRequestSize * 2);

		uint32_t sqeuence = 1; //
		uint32_t offset   = 0;
		auto end          = false;

		auto start  = std::chrono::high_resolution_clock::now();
		auto now    = std::chrono::high_resolution_clock::now();
		size_t sent = 0;

		while (offset < size) {
			temp.clear();
			temp.resize(requestHeaderSize);

			*(reinterpret_cast<uint32_t*>(&temp[0])) = sqeuence;
			*(reinterpret_cast<uint32_t*>(&temp[4])) = addr + offset;

			if (offset + maxRequestSize > size) {
				end = true;
				std::copy(data.begin() + offset, data.end(), std::back_inserter(temp));
			} else {
				std::copy(data.begin() + offset, data.begin() + offset + maxRequestSize, std::back_inserter(temp));
			}

			NetDimmPacket x = {Command::UNK1,
			                   end ? lastPacket : moreData,
			                   temp};
			sendPacket(x);

			sqeuence++;
			offset += maxRequestSize;

			sent += temp.size();
			now = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> duration = now - start;
			if (duration.count() >= 1.0 || end) {
				double speed = sent * duration.count() / 1000 / 1000;
				std::printf("%.2f (%08X / %08X)\n", speed, offset, size);
				sent = 0;
				start = std::chrono::high_resolution_clock::now();
			}
		}
	}

	// FIXME: handle when last nibble isn't multiple of 4
	auto nDownload(uint32_t addr, uint32_t length)
	{
		std::vector<uint8_t> req(requestHeaderSize, 0);

		*(reinterpret_cast<uint32_t*>(&req[0])) = addr;
		*(reinterpret_cast<uint32_t*>(&req[4])) = length;
		sendPacket({Command::REQUEST_FOR_DIMM, 0, req});
		req.clear();
		while (true) {
			auto x = recvPacket();
			if (x.data.size()) {
				std::copy(x.data.begin() + requestHeaderSize, x.data.end(), std::back_inserter(req));
			}

			// TODO: First byte is actually the sequence byte,
			// should we do something with this?
			//*(reinterpret_cast<uint32_t *>(&x.data[0]))

			length -= x.length - requestHeaderSize;
			addr += x.length - requestHeaderSize;

			if (x.flags == lastPacket || !length) {
				break;
			}
		}

		return req;
	}

	// FIXME: Add proper length limits, it's not 'free'
	auto nPeek(uint32_t addr, uint8_t length)
	{
		std::vector<uint8_t> req(4, 0);
		*(reinterpret_cast<uint32_t*>(&req[0])) = addr;

		if (length == 0x10) {
			sendPacket({Command::PeekHost16, 0, req});
		} else {
			req.resize(8);
			*(reinterpret_cast<uint32_t*>(&req[4])) = length;
			sendPacket({Command::PEEK_HOST, 0, req});
		}
		req.clear();

		req = recvPacket().data;
		std::vector<uint8_t> out = {};
		std::copy(req.begin() + 4, req.end(), std::back_inserter(out));
		return out;
	}

	// FIXME: Add proper length limits, it's not 'free'
	void nPoke(uint32_t addr, const std::vector<uint8_t>& value)
	{
		std::vector<uint8_t> req(4, 0);
		*(reinterpret_cast<uint32_t*>(&req[0])) = addr;

		if (value.size() == 0x10) {
			std::copy(value.begin(), value.end(), std::back_inserter(req));
			sendPacket({Command::PokeHost16, 0, req});
		} else {
			req.resize(8);
			*(reinterpret_cast<uint32_t*>(&req[4])) = static_cast<uint32_t>(value.size());
			std::copy(value.begin(), value.end(), std::back_inserter(req));
			sendPacket({Command::POKE_HOST, 0, req});
		}
	}

	auto nControlRead(const uint32_t offset)
	{
		std::vector<uint8_t> req = {};
		constexpr auto maxControlOffset = 0x86F0;
		if (offset >= maxControlOffset) {
			return req;
		}

		req.resize(sizeof(uint32_t));
		*(reinterpret_cast<uint32_t*>(&req[0])) = offset;
		sendPacket({Command::ControlRead, 0, req});
		req.clear();

		auto x = recvPacket();
		if (x.data.empty() || x.data.size() < 8) {
			return req;
		}

		std::copy(x.data.begin(), x.data.end() - sizeof(uint32_t), std::back_inserter(req));

		return req;
	}

	// -----

	static constexpr auto port = 10703;
	std::atomic_bool keepAlive = true;

	static constexpr uint16_t maxRequestSize   = 0x4000; // Naomi appears to be 0x4000?
	static constexpr uint8_t requestHeaderSize = 10;
	static constexpr uint8_t moreData          = 0x80;
	static constexpr uint8_t lastPacket        = 0x81;

	// Reading/Writing from this location accesses the on-board RAM of the net board
	static constexpr uint32_t netMemMask = 0xAC800000;
private:
	sockpp::tcp_connector connection = {};
	const std::string_view address   = {};
	std::chrono::seconds timeout     = {};

	void KeepAliveThread()
	{
		const auto packet = NetDimmPacket(Command::NOP, 0);
		auto con          = connection.clone();
		while (keepAlive) {
			// sendPacket(packet, &con);
			std::this_thread::sleep_for(timeout);
		}
	}
};
