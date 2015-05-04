// Copyright (c) 2015, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
package org.kududb.client;

import kudu.WireProtocol;
import kudu.consensus.Metadata;
import kudu.master.Master;

/**
 * Response for {@link GetMasterRegistrationRequest}.
 */
public class GetMasterRegistrationResponse extends KuduRpcResponse {

  private final Metadata.QuorumPeerPB.Role role;
  private final WireProtocol.ServerRegistrationPB serverRegistration;
  private final WireProtocol.NodeInstancePB instanceId;

  /**
   * Describes a response to a {@link GetMasterRegistrationRequest}, built from
   * {@link Master.GetMasterRegistrationResponsePB}.
   *
   * @param role Master's role in the quorum.
   * @param serverRegistration server registration (RPC and HTTP addresses) for this master.
   * @param instanceId Node instance (permanent uuid and
   */
  public GetMasterRegistrationResponse(long elapsedMillis, String tsUUID,
                                       Metadata.QuorumPeerPB.Role role,
                                       WireProtocol.ServerRegistrationPB serverRegistration,
                                       WireProtocol.NodeInstancePB instanceId) {
    super(elapsedMillis, tsUUID);
    this.role = role;
    this.serverRegistration = serverRegistration;
    this.instanceId = instanceId;
  }

  /**
   * Returns this master's role in the quorum.
   *
   * @see Metadata.QuorumPeerPB.Role
   * @return Node's role in the cluster, or FOLLOWER if the node is not initialized.
   */
  public Metadata.QuorumPeerPB.Role getRole() {
    return role;
  }

  /**
   * Returns the server registration (list of RPC and HTTP ports) for this master.
   *
   * @return The {@link WireProtocol.ServerRegistrationPB} object for this master.
   */
  public WireProtocol.ServerRegistrationPB getServerRegistration() {
    return serverRegistration;
  }

  /**
   * The node instance (initial sequence number and permanent uuid) for this master.
   *
   * @return The {@link WireProtocol.NodeInstancePB} object for this master.
   */
  public WireProtocol.NodeInstancePB getInstanceId() {
    return instanceId;
  }

  @Override
  public String toString() {
    return "GetMasterRegistrationResponse{" +
        "role=" + role +
        ", serverRegistration=" + serverRegistration +
        ", instanceId=" + instanceId +
        '}';
  }
}