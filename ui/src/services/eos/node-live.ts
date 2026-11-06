import { eosRequest, type EosControlReply } from './control';
/** EOS alone supplies a node's live adapter status; a graph profile is never health. */
export async function getNodeLive(path: string): Promise<{ status: string; health: string }> {
  const [status, health] = await Promise.all([
    eosRequest<EosControlReply & { status: { state: string } }>({ op: 'status', node_path: path }),
    eosRequest<EosControlReply & { health: { state: string } }>({ op: 'health', node_path: path }),
  ]);
  return { status: status.status.state, health: health.health.state };
}
