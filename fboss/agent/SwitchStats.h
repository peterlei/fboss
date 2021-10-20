/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <boost/container/flat_map.hpp>
#include <boost/noncopyable.hpp>
#include <fb303/ThreadCachedServiceData.h>
#include <chrono>
#include "fboss/agent/AggregatePortStats.h"
#include "fboss/agent/PortStats.h"
#include "fboss/agent/types.h"

namespace facebook::fboss {

class PortStats;

typedef boost::container::flat_map<PortID, std::unique_ptr<PortStats>>
    PortStatsMap;
using AggregatePortStatsMap = boost::container::
    flat_map<AggregatePortID, std::unique_ptr<AggregatePortStats>>;

class SwitchStats : public boost::noncopyable {
 public:
  /*
   * The prefix to use for our counter names
   */
  static std::string kCounterPrefix;

  SwitchStats();

  /*
   * Return the PortStats object for the given PortID.
   */
  PortStats* FOLLY_NULLABLE port(PortID portID);

  AggregatePortStats* FOLLY_NULLABLE
  aggregatePort(AggregatePortID aggregatePortID);

  /*
   * Getters.
   */
  PortStatsMap* getPortStats() {
    return &ports_;
  }
  const PortStatsMap* getPortStats() const {
    return &ports_;
  }

  // Create a PortStats object for the given PortID
  PortStats* createPortStats(PortID portID, std::string portName);
  AggregatePortStats* createAggregatePortStats(
      AggregatePortID id,
      std::string name);

  void deletePortStats(PortID portID) {
    ports_.erase(portID);
  }

  void trappedPkt() {
    trapPkts_->addValue(1);
  }
  void pktDropped() {
    trapPktDrops_->addValue(1);
  }
  void pktBogus() {
    trapPktBogus_->addValue(1);
    trapPktDrops_->addValue(1);
  }
  void pktError() {
    trapPktErrors_->addValue(1);
    trapPktDrops_->addValue(1);
  }
  void pktUnhandled() {
    trapPktUnhandled_->addValue(1);
    trapPktDrops_->addValue(1);
  }
  void pktToHost(uint32_t bytes) {
    trapPktToHost_->addValue(1);
    trapPktToHostBytes_->addValue(bytes);
  }
  void pktFromHost(uint32_t bytes) {
    pktFromHost_->addValue(1);
    pktFromHostBytes_->addValue(bytes);
  }

  void arpPkt() {
    trapPktArp_->addValue(1);
  }
  void arpUnsupported() {
    arpUnsupported_->addValue(1);
    trapPktDrops_->addValue(1);
  }
  void arpNotMine() {
    arpNotMine_->addValue(1);
    trapPktDrops_->addValue(1);
  }
  void arpRequestRx() {
    arpRequestsRx_->addValue(1);
  }
  void arpRequestTx() {
    arpRequestsTx_->addValue(1);
  }
  void arpReplyRx() {
    arpRepliesRx_->addValue(1);
  }
  void arpReplyTx() {
    arpRepliesTx_->addValue(1);
  }
  void arpBadOp() {
    arpBadOp_->addValue(1);
    trapPktDrops_->addValue(1);
  }

  void ipv6NdpPkt() {
    trapPktNdp_->addValue(1);
  }
  void ipv6NdpBad() {
    ipv6NdpBad_->addValue(1);
    trapPktDrops_->addValue(1);
  }

  void dhcpV4Pkt() {
    dhcpV4Pkt_->addValue(1);
  }

  void dhcpV6Pkt() {
    dhcpV6Pkt_->addValue(1);
  }

  void ipv4Rx() {
    ipv4Rx_->addValue(1);
  }
  void ipv4TooSmall() {
    ipv4TooSmall_->addValue(1);
  }
  void ipv4WrongVer() {
    ipv4WrongVer_->addValue(1);
  }
  void ipv4Nexthop() {
    ipv4Nexthop_->addValue(1);
  }
  void ipv4Mine() {
    ipv4Mine_->addValue(1);
  }
  void ipv4NoArp() {
    ipv4NoArp_->addValue(1);
  }
  void ipv4TtlExceeded() {
    ipv4TtlExceeded_->addValue(1);
  }

  void ipv6HopExceeded() {
    ipv6HopExceeded_->addValue(1);
  }

  void udpTooSmall() {
    udpTooSmall_->addValue(1);
  }

  void dhcpV4BadPkt() {
    dhcpV4BadPkt_->addValue(1);
    dhcpV4DropPkt_->addValue(1);
    trapPktDrops_->addValue(1);
  }

  void dhcpV4DropPkt() {
    dhcpV4DropPkt_->addValue(1);
    trapPktDrops_->addValue(1);
  }

  void dhcpV6BadPkt() {
    dhcpV6BadPkt_->addValue(1);
    dhcpV6DropPkt_->addValue(1);
    trapPktDrops_->addValue(1);
  }

  void dhcpV6DropPkt() {
    dhcpV6DropPkt_->addValue(1);
    trapPktDrops_->addValue(1);
  }

  void addRouteV4() {
    addRouteV4_->addValue(1);
  }
  void addRouteV6() {
    addRouteV6_->addValue(1);
  }
  void delRouteV4() {
    delRouteV4_->addValue(1);
  }
  void delRouteV6() {
    delRouteV6_->addValue(1);
  }

  void addRoutesV4(uint64_t routeCount) {
    addRouteV4_->addValue(routeCount);
  }
  void addRoutesV6(uint64_t routeCount) {
    addRouteV6_->addValue(routeCount);
  }
  void delRoutesV4(uint64_t routeCount) {
    delRouteV4_->addValue(routeCount);
  }
  void delRoutesV6(uint64_t routeCount) {
    delRouteV6_->addValue(routeCount);
  }

  void ipv4DstLookupFailure() {
    dstLookupFailureV4_->addValue(1);
    dstLookupFailure_->addValue(1);
  }

  void ipv6DstLookupFailure() {
    dstLookupFailureV6_->addValue(1);
    dstLookupFailure_->addValue(1);
  }

  void stateUpdate(std::chrono::microseconds us) {
    updateState_.addValue(us.count());
  }

  void routeUpdate(std::chrono::microseconds us, uint64_t routes) {
    // As syncFib() could include no routes.
    if (routes == 0) {
      routes = 1;
    }
    routeUpdate_.addRepeatedValue(us.count() / routes, routes);
  }

  void bgHeartbeatDelay(int delay) {
    bgHeartbeatDelay_.addValue(delay);
  }

  void updHeartbeatDelay(int delay) {
    updHeartbeatDelay_.addValue(delay);
  }

  void packetTxHeartbeatDelay(int value) {
    packetTxHeartbeatDelay_.addValue(value);
  }

  void lacpHeartbeatDelay(int value) {
    lacpHeartbeatDelay_.addValue(value);
  }

  void neighborCacheHeartbeatDelay(int value) {
    neighborCacheHeartbeatDelay_.addValue(value);
  }

  void bgEventBacklog(int value) {
    bgEventBacklog_.addValue(value);
  }

  void updEventBacklog(int value) {
    updEventBacklog_.addValue(value);
  }

  void lacpEventBacklog(int value) {
    lacpEventBacklog_.addValue(value);
  }

  void packetTxEventBacklog(int value) {
    packetTxEventBacklog_.addValue(value);
  }

  void neighborCacheEventBacklog(int value) {
    neighborCacheEventBacklog_.addValue(value);
  }

  void linkStateChange() {
    linkStateChange_->addValue(1);
  }

  void pcapDistFailure() {
    pcapDistFailure_.incrementValue(1);
  }

  void updateStatsException() {
    updateStatsExceptions_->addValue(1);
  }

  void pktTooBig() {
    trapPktTooBig_->addValue(1);
  }

  void LldpRecvdPkt() {
    LldpRecvdPkt_->addValue(1);
  }
  void LldpBadPkt() {
    LldpBadPkt_->addValue(1);
  }
  void LldpValidateMisMatch() {
    LldpValidateMisMatch_->addValue(1);
  }
  void LldpNeighborsSize(int value) {
    LldpNeighborsSize_->addValue(value);
  }
  void LacpRxTimeouts() {
    LacpRxTimeouts_->addValue(1);
  }
  void LacpMismatchPduTeardown() {
    LacpMismatchPduTeardown_->addValue(1);
  }

  void MkPduRecvdPkt() {
    MkPduRecvdPkts_->addValue(1);
  }
  void MkPduSendPkt() {
    MkPduSendPkts_->addValue(1);
  }
  void MkPduSendFailure() {
    MkPduSendFailure_->addValue(1);
  }
  void MkPduPortNotRegistered() {
    MkPduPortNotRegistered_->addValue(1);
  }
  void MKAServiceSendFailue() {
    MKAServiceSendFailure_->addValue(1);
  }
  void MKAServiceSendSuccess() {
    MKAServiceSendSuccess_->addValue(1);
  }
  void MKAServiceRecvSuccess() {
    MKAServiceRecvSuccess_->addValue(1);
  }
  void PfcDeadlockDetectionCount() {
    pfcDeadlockDetectionCount_->addValue(1);
  }
  void PfcDeadlockRecoveryCount() {
    pfcDeadlockRecoveryCount_->addValue(1);
  }

 private:
  // Forbidden copy constructor and assignment operator
  SwitchStats(SwitchStats const&) = delete;
  SwitchStats& operator=(SwitchStats const&) = delete;

  typedef fb303::ThreadCachedServiceData::ThreadLocalStatsMap
      ThreadLocalStatsMap;
  typedef fb303::ThreadCachedServiceData::TLTimeseries TLTimeseries;
  typedef fb303::ThreadCachedServiceData::TLHistogram TLHistogram;
  typedef fb303::ThreadCachedServiceData::TLCounter TLCounter;

  using TLTimeseriesPtr = std::unique_ptr<TLTimeseries>;

  explicit SwitchStats(ThreadLocalStatsMap* map);

  // Total number of trapped packets
  TLTimeseriesPtr trapPkts_;
  // Number of trapped packets that were intentionally dropped.
  TLTimeseriesPtr trapPktDrops_;
  // Malformed packets received
  TLTimeseriesPtr trapPktBogus_;
  // Number of times the controller encountered an error trying to process
  // a packet.
  TLTimeseriesPtr trapPktErrors_;
  // Trapped packets that the controller didn't know how to handle.
  TLTimeseriesPtr trapPktUnhandled_;
  // Trapped packets forwarded to host
  TLTimeseriesPtr trapPktToHost_;
  // Trapped packets forwarded to host in bytes
  TLTimeseriesPtr trapPktToHostBytes_;
  // Packets sent by host
  TLTimeseriesPtr pktFromHost_;
  // Packets sent by host in bytes
  TLTimeseriesPtr pktFromHostBytes_;

  // ARP Packets
  TLTimeseriesPtr trapPktArp_;
  // ARP packets dropped due to unsupported hardware or protocol settings.
  TLTimeseriesPtr arpUnsupported_;
  // ARP packets ignored because we were not the target.
  TLTimeseriesPtr arpNotMine_;
  // ARP requests destined for us
  TLTimeseriesPtr arpRequestsRx_;
  // ARP replies destined for us
  TLTimeseriesPtr arpRepliesRx_;
  // ARP requests sent from us
  TLTimeseriesPtr arpRequestsTx_;
  // ARP replies sent from us
  TLTimeseriesPtr arpRepliesTx_;
  // ARP packets with an unknown op field
  TLTimeseriesPtr arpBadOp_;

  // IPv6 Neighbor Discovery Protocol packets
  TLTimeseriesPtr trapPktNdp_;
  TLTimeseriesPtr ipv6NdpBad_;

  // IPv4 Packets
  TLTimeseriesPtr ipv4Rx_;
  // IPv4 packets dropped due to smaller packet size
  TLTimeseriesPtr ipv4TooSmall_;
  // IPv4 packets dropped due to wrong version number
  TLTimeseriesPtr ipv4WrongVer_;
  // IPv4 packets received for resolving nexthop
  TLTimeseriesPtr ipv4Nexthop_;
  // IPv4 packets received for myself
  TLTimeseriesPtr ipv4Mine_;
  // IPv4 packets received, but not able to send out ARP request
  TLTimeseriesPtr ipv4NoArp_;
  // IPv4 TTL exceeded
  TLTimeseriesPtr ipv4TtlExceeded_;

  // IPv6 hop count exceeded
  TLTimeseriesPtr ipv6HopExceeded_;

  // UDP packets dropped due to smaller packet size
  TLTimeseriesPtr udpTooSmall_;

  // Total DHCP packets received
  TLTimeseriesPtr dhcpV4Pkt_;
  /*
   *  Consolidate DHCP v4 drops under 2 heads
   *  a) Bad dhcp packet receive
   *  b) Catch all dhcp packet drop. This includes a) but also includes
   *  cases where a dhcp relay was not configured for this VLAN
   */
  // DHCPv4 packets dropped due to packet being malformed
  TLTimeseriesPtr dhcpV4BadPkt_;
  // DHCPv4 packets dropped
  TLTimeseriesPtr dhcpV4DropPkt_;
  // stats for dhcpV6
  TLTimeseriesPtr dhcpV6Pkt_;
  TLTimeseriesPtr dhcpV6BadPkt_;
  TLTimeseriesPtr dhcpV6DropPkt_;

  /**
   * Routes add/delete stats
   *
   * Modification is treated as add.
   */
  TLTimeseriesPtr addRouteV4_;
  TLTimeseriesPtr addRouteV6_;
  TLTimeseriesPtr delRouteV4_;
  TLTimeseriesPtr delRouteV6_;

  TLTimeseriesPtr dstLookupFailureV4_;
  TLTimeseriesPtr dstLookupFailureV6_;
  TLTimeseriesPtr dstLookupFailure_;

  /**
   * Histogram for time used for SwSwitch::updateState() (in ms)
   */
  TLHistogram updateState_;

  /**
   * Histogram for time used for route update (in microsecond)
   */
  TLHistogram routeUpdate_;

  /**
   * Background thread heartbeat delay (ms)
   */
  TLHistogram bgHeartbeatDelay_;

  /**
   * Update thread heartbeat delay (ms)
   */
  TLHistogram updHeartbeatDelay_;
  /**
   * Fboss packet Tx thread heartbeat delay (ms)
   */
  TLHistogram packetTxHeartbeatDelay_;
  /**
   * LACP thread heartbeat delay in milliseconds
   */
  TLHistogram lacpHeartbeatDelay_;
  /**
   * Arp Cache thread heartbeat delay in milliseconds
   */
  TLHistogram neighborCacheHeartbeatDelay_;

  /**
   * Number of events queued in background thread
   */
  TLHistogram bgEventBacklog_;

  /**
   * Number of events queued in update thread
   */
  TLHistogram updEventBacklog_;
  /**
   * Number of events queued in fboss packet TX thread
   */
  TLHistogram packetTxEventBacklog_;

  TLHistogram lacpEventBacklog_;
  /**
   * Number of events queued in fboss Arp Cache thread
   */
  TLHistogram neighborCacheEventBacklog_;

  /**
   * Link state up/down change count
   */
  TLTimeseriesPtr linkStateChange_;

  // Individual port stats objects, indexed by PortID
  PortStatsMap ports_;

  AggregatePortStatsMap aggregatePortIDToStats_;

  // Number of packets dropped by the PCAP distribution service
  TLCounter pcapDistFailure_;

  // Number of failed updateStats callbacks do to exceptions.
  TLTimeseriesPtr updateStatsExceptions_;

  // Number of packet too big ICMPv6 triggered
  TLTimeseriesPtr trapPktTooBig_;

  // Number of LLDP packets.
  TLTimeseriesPtr LldpRecvdPkt_;
  // Number of bad LLDP packets.
  TLTimeseriesPtr LldpBadPkt_;
  // Number of LLDP packets that did not match configured, expected values.
  TLTimeseriesPtr LldpValidateMisMatch_;
  // Number of LLDP Neighbors.
  TLTimeseriesPtr LldpNeighborsSize_;

  // Number of LACP Rx timeouts
  TLTimeseriesPtr LacpRxTimeouts_;
  // Number of LACP session teardown due to mismatching PDUs
  TLTimeseriesPtr LacpMismatchPduTeardown_;
  // Number of MkPdu Received.
  TLTimeseriesPtr MkPduRecvdPkts_;
  // Number of MkPdu Send.
  TLTimeseriesPtr MkPduSendPkts_;
  // Number of MkPdu Send Failure
  TLTimeseriesPtr MkPduSendFailure_;
  // Number of pkt recv when port is not registered.
  TLTimeseriesPtr MkPduPortNotRegistered_;
  // Number of Send Pkt to MkaService Failure.
  TLTimeseriesPtr MKAServiceSendFailure_;
  // Number of Pkts Send to MKAService.
  TLTimeseriesPtr MKAServiceSendSuccess_;
  // Number of pkts recvd from MkaService.
  TLTimeseriesPtr MKAServiceRecvSuccess_;
  // Number of timers pfc deadlock watchodg was hit
  TLTimeseriesPtr pfcDeadlockDetectionCount_;
  // Number of timers pfc deadlock recovery hit
  TLTimeseriesPtr pfcDeadlockRecoveryCount_;

  // TODO: use template parameter pack once FSDB is open sourced instead of
  // method overloading
  static std::unique_ptr<TLTimeseries> makeTLTimeseries(
      ThreadLocalStatsMap* map,
      std::string&& key,
      fb303::ExportType exportType);
  static std::unique_ptr<TLTimeseries> makeTLTimeseries(
      ThreadLocalStatsMap* map,
      std::string&& key,
      fb303::ExportType exportType1,
      fb303::ExportType exportType2);
};

} // namespace facebook::fboss
