#include "AccountService.h"
#include "LoggerComponent.h"
#include "NetworkConnection.h"
#include "Packet.h"
#include "Settings.h"
#include "World.h"

#include <regex>
#include <sstream>
#include <string>
#include <openssl/rsa.h>

extern std::weak_ptr<Settings> g_settings;
extern std::map<uint32_t, std::shared_ptr<World>> g_worlds;
extern LoggerComponent *g_logger;
extern RSA *g_rsa;

AccountService::AccountService()
{
	std::regex regex("[^[:digit:]]");
	if (auto settings = g_settings.lock()) {
		requiredClientVersionString = settings->getString("client_version");
		requiredClientVersion = (uint16_t)std::stoi(std::regex_replace(requiredClientVersionString, regex, ""));
	}
}

AccountService::~AccountService()
{
}

unsigned short AccountService::getBindPort()
{
	if (auto settings = g_settings.lock())
		return (unsigned short)settings->getUnsigned("login_port");

	return 0U;
}

std::string AccountService::getBindAddress()
{
	if (auto settings = g_settings.lock())
		return settings->getString("login_address");

	return "localhost";
}

bool AccountService::needChecksum()
{
	return true;
}

bool AccountService::canHandle(NetworkConnectionPtr connection, PacketPtr packet)
{
	return packet->peek<uint8_t>() == 0x01;
}

bool AccountService::handleFirst(NetworkConnectionPtr connection, PacketPtr packet)
{
	uint16_t os = packet->pop<uint16_t>();
	uint16_t version = packet->pop<uint16_t>();

	if (version >= 971)
		packet->skip(17);
	else
		packet->skip(12);

	if (version <= 760) {
		disconnectClient(connection, 0x0a, "This server requires version " + requiredClientVersionString + ".");
		return false;
	}

	if (!RSAdecrypt(connection, packet)) {
		return false;
	}

	std::array<uint32_t, 4> keys = { packet->pop<uint32_t>(), packet->pop<uint32_t>(), packet->pop<uint32_t>(), packet->pop<uint32_t>() };
	connection->setKeys(keys);

	std::string account = packet->pop<std::string>();
	std::string password = packet->pop<std::string>();

	// TODO: Validate account and password
	PacketPtr outPacket(new Packet);

	// Send MOTD
	outPacket->push<uint8_t>(0x14).push<std::string>("1\nPhoenixTibiaServer v0.1");

	// Send character list
	outPacket->push<uint8_t>(0x64)
		// Add Worlds
		.push<uint8_t>((uint8_t)g_worlds.size());
	for (auto &&world : g_worlds) {
		outPacket->push<uint8_t>((uint8_t)world.first)
			.push<std::string>(world.second->name())
			.push<std::string>(world.second->endpoint().address().to_string())
			.push<uint16_t>(world.second->endpoint().port())
			.push<uint8_t>(0);
	}

	// Add characters
	outPacket->push<uint8_t>((uint8_t)g_worlds.size());
	for (auto &&world : g_worlds) {
		outPacket->push<uint8_t>((uint8_t)world.first)
			.push<std::string>("Test on " + world.second->name());
	}

	// Add premium days
	outPacket->push<uint16_t>(0xffff);

	connection->send(outPacket);

	return false;
}

void AccountService::disconnectClient(NetworkConnectionPtr connection, uint8_t error, const std::string& message)
{
	PacketPtr packet(new Packet);

	packet->push(error).push(message);

	connection->send(packet);
}

bool AccountService::RSAdecrypt(NetworkConnectionPtr connection, PacketPtr packet)
{
	if (packet->size() - packet->pos() != 128) {
		if (g_logger) {
			std::stringstream ss;
			ss << "Client " + connection->socket().remote_endpoint().address().to_string() + " sent invalid packet size";
			g_logger->log(LogLevel::Information, ss.str());
		}

		return false;
	}

	size_t pos = packet->pos();
	std::shared_ptr<uint8_t> encData(new uint8_t[128]);
	packet->get(128, encData.get()).pos(pos);
	if (RSA_private_decrypt(128, encData.get(), packet->data() + pos, g_rsa, RSA_NO_PADDING) == -1 || packet->pop<uint8_t>() != 0) {
		if (g_logger) {
			std::stringstream ss;
			ss << "Client " + connection->socket().remote_endpoint().address().to_string() + " sent invalid RSA encrypted data";
			g_logger->log(LogLevel::Information, ss.str());
		}

		return false;
	}

	return true;
}