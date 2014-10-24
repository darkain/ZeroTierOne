/*
 * ZeroTier One - Global Peer to Peer Ethernet
 * Copyright (C) 2011-2014  ZeroTier Networks LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <string>
#include <map>
#include <vector>

#include "node/Constants.hpp"
#include "node/Node.hpp"
#include "node/Utils.hpp"
#include "node/Address.hpp"
#include "node/Identity.hpp"
#include "node/Thread.hpp"
#include "node/CMWC4096.hpp"
#include "node/Dictionary.hpp"

#include "testnet/SimNet.hpp"
#include "testnet/SimNetSocketManager.hpp"
#include "testnet/TestEthernetTap.hpp"
#include "testnet/TestEthernetTapFactory.hpp"
#include "testnet/TestRoutingTable.hpp"

#ifdef __WINDOWS__
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

using namespace ZeroTier;

class SimNode
{
public:
	SimNode(SimNet &net,const std::string &hp,const char *rootTopology,bool issn,const InetAddress &addr) :
		home(hp),
		tapFactory(),
		routingTable(),
		socketManager(net.newEndpoint(addr)),
		node(home.c_str(),&tapFactory,&routingTable,socketManager,false,rootTopology),
		reasonForTermination(Node::NODE_RUNNING),
		supernode(issn)
	{
		thread = Thread::start(this);
	}

	~SimNode()
	{
		node.terminate(Node::NODE_NORMAL_TERMINATION,"SimNode shutdown");
		Thread::join(thread);
	}

	void threadMain()
		throw()
	{
		reasonForTermination = node.run();
	}

	std::string home;
	TestEthernetTapFactory tapFactory;
	TestRoutingTable routingTable;
	SimNetSocketManager *socketManager;
	Node node;
	Node::ReasonForTermination reasonForTermination;
	bool supernode;
	Thread thread;
};

static std::string basePath;
static SimNet net;
static std::map< Address,SimNode * > nodes;
static std::map< InetAddress,Address > usedIps;
static CMWC4096 prng;
static std::string rootTopology;

// Converts an address into a fake IP not already claimed.
// Be sure to call only once, as this claims the IP before returning it.
static InetAddress inetAddressFromZeroTierAddress(const Address &addr)
{
	uint32_t ip = (uint32_t)(addr.toInt() & 0xffffffff);
	for(;;) {
		if (((ip >> 24) & 0xff) >= 240) {
			ip &= 0x00ffffff;
			ip |= (((ip >> 24) & 0xff) % 240) << 24;
		}
		if (((ip >> 24) & 0xff) == 0)
			ip |= 0x01000000;
		if (((ip & 0xff) == 0)||((ip & 0xff) == 255))
			ip ^= 0x00000001;
		InetAddress inaddr(Utils::hton(ip),9993);
		if (usedIps.find(inaddr) == usedIps.end()) {
			usedIps[inaddr] = addr;
			return inaddr;
		}
		++ip; // keep looking sequentially for an unclaimed IP
	}
}

static Identity makeNodeHome(bool super)
{
	Identity id;
	id.generate();

	std::string path(basePath + ZT_PATH_SEPARATOR_S + (super ? "S" : "N") + id.address().toString());

#ifdef __WINDOWS__
	CreateDirectoryA(path.c_str(),NULL);
#else
	mkdir(path.c_str(),0700);
#endif

	if (!Utils::writeFile((path + ZT_PATH_SEPARATOR_S + "identity.secret").c_str(),id.toString(true)))
		return Identity();
	if (!Utils::writeFile((path + ZT_PATH_SEPARATOR_S + "identity.public").c_str(),id.toString(false)))
		return Identity();

	return id;
}

// Instantiates supernodes by scanning for S########## subdirectories
static std::vector<Address> initSupernodes()
{
	Dictionary supernodes;
	std::vector< std::pair<Identity,InetAddress> > snids;
	std::map<std::string,bool> dir(Utils::listDirectory(basePath.c_str()));

	for(std::map<std::string,bool>::iterator d(dir.begin());d!=dir.end();++d) {
		if ((d->first.length() == 11)&&(d->second)&&(d->first[0] == 'S')) {
			std::string idbuf;
			if (Utils::readFile((basePath + ZT_PATH_SEPARATOR_S + d->first + ZT_PATH_SEPARATOR_S + "identity.public").c_str(),idbuf)) {
				Identity id(idbuf);
				if (id) {
					InetAddress inaddr(inetAddressFromZeroTierAddress(id.address()));
					snids.push_back(std::pair<Identity,InetAddress>(id,inaddr));

					Dictionary snd;
					snd["id"] = id.toString(false);
					snd["udp"] = inaddr.toString();
					snd["desc"] = id.address().toString();
					snd["dns"] = inaddr.toIpString();
					supernodes[id.address().toString()] = snd.toString();
				}
			}
		}
	}

	Dictionary rtd;
	rtd["supernodes"] = supernodes.toString();
	rtd["noupdate"] = "1";
	rootTopology = rtd.toString();

	std::vector<Address> newNodes;

	for(std::vector< std::pair<Identity,InetAddress> >::iterator i(snids.begin());i!=snids.end();++i) {
		SimNode *n = new SimNode(net,(basePath + ZT_PATH_SEPARATOR_S + "S" + i->first.address().toString()),rootTopology.c_str(),true,i->second);
		nodes[i->first.address()] = n;
		newNodes.push_back(i->first.address());
	}

	return newNodes;
}

// Instantiates any not-already-instantiated regular nodes
static std::vector<Address> scanForNewNodes()
{
	std::vector<Address> newNodes;
	std::map<std::string,bool> dir(Utils::listDirectory(basePath.c_str()));

	for(std::map<std::string,bool>::iterator d(dir.begin());d!=dir.end();++d) {
		if ((d->first.length() == 11)&&(d->second)&&(d->first[0] == 'N')) {
			Address na(d->first.c_str() + 1);
			if (nodes.find(na) == nodes.end()) {
				InetAddress inaddr(inetAddressFromZeroTierAddress(na));

				SimNode *n = new SimNode(net,(basePath + ZT_PATH_SEPARATOR_S + d->first),rootTopology.c_str(),false,inaddr);
				nodes[na] = n;

				newNodes.push_back(na);
			}
		}
	}

	return newNodes;
}

static void doHelp(const std::vector<std::string> &cmd)
{
	printf("---------- help"ZT_EOL_S);
	printf("---------- mksn <number of supernodes>"ZT_EOL_S);
	printf("---------- mkn <number of normal nodes>"ZT_EOL_S);
	printf("---------- list"ZT_EOL_S);
	printf("---------- join <address/*> <network ID>"ZT_EOL_S);
	printf("---------- leave <address/*> <network ID>"ZT_EOL_S);
	printf("---------- listnetworks <address/*>"ZT_EOL_S);
	printf("---------- listpeers <address/*>"ZT_EOL_S);
	printf("---------- alltoall"ZT_EOL_S);
	printf("---------- quit"ZT_EOL_S);
}

static void doMKSN(const std::vector<std::string> &cmd)
{
	if (cmd.size() < 2) {
		doHelp(cmd);
		return;
	}
	if (nodes.size() > 0) {
		printf("---------- mksn error: mksn can only be called once (network already exists)"ZT_EOL_S);
		return;
	}

	int count = Utils::strToInt(cmd[1].c_str());
	for(int i=0;i<count;++i) {
		Identity id(makeNodeHome(true));
		printf("%s identity created"ZT_EOL_S,id.address().toString().c_str());
	}

	std::vector<Address> nodes(initSupernodes());
	for(std::vector<Address>::iterator a(nodes.begin());a!=nodes.end();++a)
		printf("%s started (supernode)"ZT_EOL_S,a->toString().c_str());

	printf("---------- root topology is: %s"ZT_EOL_S,rootTopology.c_str());
}

static void doMKN(const std::vector<std::string> &cmd)
{
	if (cmd.size() < 2) {
		doHelp(cmd);
		return;
	}
	if (nodes.size() == 0) {
		printf("---------- mkn error: use mksn to create supernodes first."ZT_EOL_S);
		return;
	}
}

static void doList(const std::vector<std::string> &cmd)
{
}

static void doJoin(const std::vector<std::string> &cmd)
{
}

static void doLeave(const std::vector<std::string> &cmd)
{
}

static void doListNetworks(const std::vector<std::string> &cmd)
{
}

static void doListPeers(const std::vector<std::string> &cmd)
{
}

static void doAllToAll(const std::vector<std::string> &cmd)
{
}

int main(int argc,char **argv)
{
	char linebuf[1024];

	if (argc <= 1) {
		fprintf(stderr,"Usage: %s <base path for temporary node home directories>"ZT_EOL_S,argv[0]);
		return 1;
	}

	basePath = argv[1];
#ifdef __WINDOWS__
	CreateDirectoryA(basePath.c_str(),NULL);
#else
	mkdir(basePath.c_str(),0700);
#endif

	printf("*** ZeroTier One Version %s -- Headless Network Simulator ***"ZT_EOL_S,Node::versionString());
	printf(ZT_EOL_S);

	{
		printf("---------- scanning '%s' for existing network..."ZT_EOL_S,basePath.c_str());
		std::vector<Address> snodes(initSupernodes());
		if (snodes.empty()) {
			printf("---------- no existing network found; use 'mksn' to create one."ZT_EOL_S);
		} else {
			for(std::vector<Address>::iterator a(snodes.begin());a!=snodes.end();++a)
				printf("%s started (supernode)"ZT_EOL_S,a->toString().c_str());
			printf("---------- root topology is: %s"ZT_EOL_S,rootTopology.c_str());
			std::vector<Address> nodes(scanForNewNodes());
			for(std::vector<Address>::iterator a(nodes.begin());a!=nodes.end();++a)
				printf("%s started (normal peer)"ZT_EOL_S,a->toString().c_str());
			printf("---------- %u peers and %u supernodes loaded!"ZT_EOL_S,(unsigned int)nodes.size(),(unsigned int)snodes.size());
		}
	}
	printf(ZT_EOL_S);

	printf("Type 'help' for help."ZT_EOL_S);
	printf(ZT_EOL_S);

	for(;;) {
		printf(">> ");
		fflush(stdout);
		if (!fgets(linebuf,sizeof(linebuf),stdin))
			break;
		std::vector<std::string> cmd(Utils::split(linebuf," \r\n\t","\\","\""));
		if (cmd.size() == 0)
			continue;

		if (cmd[0] == "quit")
			break;
		else if (cmd[0] == "help")
			doHelp(cmd);
		else if (cmd[0] == "mksn")
			doMKSN(cmd);
		else if (cmd[0] == "mkn")
			doMKN(cmd);
		else if (cmd[0] == "join")
			doJoin(cmd);
		else if (cmd[0] == "leave")
			doLeave(cmd);
		else if (cmd[0] == "listnetworks")
			doListNetworks(cmd);
		else if (cmd[0] == "listpeers")
			doListPeers(cmd);
		else if (cmd[0] == "alltoall")
			doAllToAll(cmd);
		else doHelp(cmd);
	}

	for(std::map< Address,SimNode * >::iterator n(nodes.begin());n!=nodes.end();++n) {
		printf("%s shutting down..."ZT_EOL_S,n->first.toString().c_str());
		delete n->second;
	}

	return 0;
}
