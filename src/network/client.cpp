#include "network/client.h"
#include "network/server.h"       // PacketType enum
#include "network/input_snapshot.h"
#include "core/entity_manager.h"  // TransformComponent
#include "core/types.h"           // EntityID, NULL_ENTITY
#include "simulation/mob_system.h"    // MobComponent, HealthComponent, HumanAppearance
#include "simulation/world_items.h"   // WorldItemComponent
#include "simulation/status_effects.h" // StatusEffectsComponent
#include "simulation/physics.h"       // CharacterControllerComponent, VelocityComponent, DensityComponent
#include "data/voxel_registry.h"
#include "inventory/item_registry.h"  // ItemRegistry
#include <SDL3/SDL.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

// ── Shared clothing / inhand lookup tables ─────────────────────────────────
// Mirrored from main.cpp k_clothing_map / k_inhand_map so received
// AppearanceState packets can rebuild remote-player overlays client-side.
namespace {
struct CEntry { const char* id; const char* dir; const char* name; };
static const CEntry k_clothing_map[] = {
    { "hardsuit",        "clothing/suits/spacesuit", "space"            },
    { "body_armor",      "clothing/suits/armor",     "armor"            },
    { "security_armor",  "clothing/suits/armor",     "armor_sec"        },
    { "hardsuit_helmet", "clothing/head/spacehelm",  "space"            },
    { "mining_helmet",   "clothing/head/utility",    "hardhat0_orange"  },
    { "welding_helmet",  "clothing/head/utility",    "welding"          },
    { "hard_hat",        "clothing/head/utility",    "hardhat0_yellow"  },
    { "welding_goggles", "clothing/eyes",            "welding-g"        },
    { "sunglasses",      "clothing/eyes",            "bigsunglasses"    },
    { "gas_mask",        "clothing/mask",            "gas_mask"         },
    { "jetpack",         "clothing/back",            "jetpack"          },
    { "toolbelt",        "clothing/belt",            "ebelt"            },
    { "rubber_gloves",   "clothing/hands",           "latex"            },
    { "magboots",        "clothing/feet",            "magboots0"        },
    { "boots",           "clothing/feet",            "workboots"        },
    { "headset",         "clothing/ears",            "headset"          },
};
struct IEntry { const char* id; const char* category; const char* name; };
static const IEntry k_inhand_map[] = {
    { "wrench",       "equipment/tools",   "wrench"       },
    { "screwdriver",  "equipment/tools",   "screwdriver"  },
    { "crowbar",      "equipment/tools",   "crowbar"      },
    { "wirecutters",  "equipment/tools",   "cutters"      },
    { "welder",       "equipment/tools",   "welder"       },
    { "multitool",    "equipment/tools",   "multitool"    },
    { "drill",        "equipment/tools",   "drill"        },
    { "rcd",          "equipment/tools",   "rcd"          },
    { "pickaxe",      "equipment/mining",  "pickaxe"      },
    { "shovel",       "equipment/mining",  "shovel"       },
    { "flashlight",   "items/devices",     "flashlight"   },
    { "radio",        "items/devices",     "radio"        },
    { "baseball_bat", "weapons/melee",     "baseball_bat" },
    { "katana",       "weapons/swords",    "katana"       },
    { "bow",          "weapons/bows",      "bow"          },
    { "beaker",       "items",             "beaker"       },
    { "medipen",      "equipment/medical", "medipen"      },
    { "syringe",      "equipment/medical", "syringe_0"    },
    { "scalpel",      "equipment/medical", "scalpel"      },
};
// Slot IDs that are clothing (not inhand)
static const char* k_clothing_slot_ids[] = {
    "suit","head","glasses","mask","back","belt","gloves","shoes","ears"
};
} // namespace

// ── Platform UDP socket helpers ───────────────────────────────────────────────
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using sock_t = SOCKET;
   static constexpr sock_t k_bad_sock = INVALID_SOCKET;
   static void  cli_net_close(sock_t s)  { ::closesocket(s); }
   static bool  cli_net_would_block()    { return WSAGetLastError()==WSAEWOULDBLOCK; }
   static bool  cli_net_init()           { WSADATA w; return WSAStartup(MAKEWORD(2,2),&w)==0; }
   static void  cli_net_quit()           { WSACleanup(); }
   static bool  cli_net_set_nonblock(sock_t s) { u_long v=1; return ::ioctlsocket(s,FIONBIO,&v)==0; }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
   using sock_t = int;
   static constexpr sock_t k_bad_sock = -1;
   static void  cli_net_close(sock_t s)  { ::close(s); }
   static bool  cli_net_would_block()    { return errno==EAGAIN||errno==EWOULDBLOCK; }
   static bool  cli_net_init()           { return true; }
   static void  cli_net_quit()           { }
   static bool  cli_net_set_nonblock(sock_t s) {
       int f=::fcntl(s,F_GETFL,0); return ::fcntl(s,F_SETFL,f|O_NONBLOCK)!=-1; }
#endif
static_assert(sizeof(sock_t) <= sizeof(uintptr_t));
static inline sock_t    cli_to_sock(uintptr_t u){ return static_cast<sock_t>(u); }
static inline uintptr_t cli_to_uptr(sock_t  s) { return static_cast<uintptr_t>(s); }

static void cli_send_packet(sock_t fd, const sockaddr_in& dest,
                             PacketType type, const void* data, size_t len)
{
    const size_t pkt_size = 6 + len;
    std::vector<uint8_t> pkt(pkt_size);
    uint16_t t = static_cast<uint16_t>(type);
    uint32_t l = static_cast<uint32_t>(len);
    std::memcpy(pkt.data(),     &t, 2);
    std::memcpy(pkt.data() + 2, &l, 4);
    if (data && len > 0)
        std::memcpy(pkt.data() + 6, data, len);
    ::sendto(fd, reinterpret_cast<const char*>(pkt.data()),
             static_cast<int>(pkt_size), 0,
             reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
}

// ── Client ────────────────────────────────────────────────────────────────────

Client::Client()  = default;
Client::~Client() { disconnect(); }

bool Client::connect(const char* host, uint16_t port)
{
    m_world    = std::make_unique<World>();
    m_entities = std::make_unique<EntityManager>();

    if (!cli_net_init()) { SDL_Log("Client: network init failed"); return false; }

    // Strip optional :port suffix from host string for DNS resolution
    char host_buf[256];
    SDL_strlcpy(host_buf, host, sizeof(host_buf));
    char* colon = SDL_strrchr(host_buf, ':');
    if (colon) {
        port = static_cast<uint16_t>(SDL_atoi(colon + 1));
        *colon = '\0';
    }

    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[8];
    SDL_snprintf(port_str, sizeof(port_str), "%u", port);

    addrinfo* res = nullptr;
    if (::getaddrinfo(host_buf, port_str, &hints, &res) != 0 || !res) {
        SDL_Log("Client: getaddrinfo(%s) failed", host_buf);
        cli_net_quit();
        return false;
    }

    sock_t fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == k_bad_sock) {
        SDL_Log("Client: socket() failed");
        ::freeaddrinfo(res);
        cli_net_quit();
        return false;
    }

    sockaddr_in srv{};
    std::memcpy(&srv, res->ai_addr, sizeof(sockaddr_in));
    srv.sin_port = htons(port);
    ::freeaddrinfo(res);

    m_server_ip   = srv.sin_addr.s_addr;
    m_server_port = port;
    cli_net_set_nonblock(fd);
    m_socket = cli_to_uptr(fd);

    SDL_Log("Client: connecting to %s:%u ...", host_buf, port);

    // Build Connect payload with appearance data:
    // [eye_r:1][eye_g:1][eye_b:1][hair_len:1][hair:N][hair_r:1][hair_g:1][hair_b:1]
    {
        std::vector<uint8_t> app_payload;
        app_payload.push_back(m_appearance.eye_r);
        app_payload.push_back(m_appearance.eye_g);
        app_payload.push_back(m_appearance.eye_b);
        uint8_t hair_len = static_cast<uint8_t>(m_appearance.hair_file.size());
        app_payload.push_back(hair_len);
        app_payload.insert(app_payload.end(),
                           m_appearance.hair_file.begin(),
                           m_appearance.hair_file.end());
        app_payload.push_back(m_appearance.hair_r);
        app_payload.push_back(m_appearance.hair_g);
        app_payload.push_back(m_appearance.hair_b);
        cli_send_packet(fd, srv, PacketType::Connect,
                        app_payload.data(), app_payload.size());
    }

    // Wait up to 5 s for SpawnInfo
    uint64_t deadline = SDL_GetTicks() + 5000;
    static uint8_t buf[65536];
    while (SDL_GetTicks() < deadline) {
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof(from);
#else
        socklen_t from_len = sizeof(from);
#endif
        int n = ::recvfrom(fd, reinterpret_cast<char*>(buf), (int)sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 6) { SDL_Delay(5); continue; }

        uint16_t type_raw; uint32_t payload_len;
        std::memcpy(&type_raw,    buf,     2);
        std::memcpy(&payload_len, buf + 2, 4);
        auto type = static_cast<PacketType>(type_raw);
        if (static_cast<int>(n) < 6 + (int)payload_len) { SDL_Delay(5); continue; }
        const uint8_t* payload = buf + 6;

        if (type == PacketType::SpawnInfo && payload_len >= 16) {
            uint32_t eid_raw; float sx, sy, sz;
            std::memcpy(&eid_raw, payload,      4);
            std::memcpy(&sx,      payload +  4, 4);
            std::memcpy(&sy,      payload +  8, 4);
            std::memcpy(&sz,      payload + 12, 4);

            m_local_player = static_cast<EntityID>(eid_raw);
            m_entities->adopt(m_local_player);
            TransformComponent tr{};
            tr.pos = tr.prev_pos = {sx, sy, sz};
            m_entities->add_component<TransformComponent>(m_local_player, tr);

            // HumanAppearance for rendering the local player's body
            HumanAppearance app{};
            HumanOverlay base_layer{};
            base_layer.sprite_dir = "bodyparts_greyscale";
            base_layer.prefix     = "human";
            base_layer.gender     = "_m";
            base_layer.tint       = {255, 255, 255, 255};
            app.layers.push_back(base_layer);
            app.dirty = true;
            m_entities->add_component<HumanAppearance>(m_local_player, app);

            m_connected = true;
            SDL_Log("Client: connected! player entity %u at (%.1f,%.1f,%.1f)",
                    eid_raw, sx, sy, sz);
            return true;
        }
        if (type == PacketType::ChunkData)
            on_chunk_data(payload, payload_len);
        else if (type == PacketType::EntityState)
            on_entity_state(payload, payload_len);
        else if (type == PacketType::EntitySpawn)
            on_entity_spawn(payload, payload_len);
        else
            SDL_Log("Client: received unknown packet type %u during handshake", static_cast<unsigned>(type));
    }

    SDL_Log("Client: timeout waiting for server at %s:%u", host_buf, port);
    cli_net_close(fd);
    m_socket = uintptr_t(-1);
    cli_net_quit();
    return false;
}

void Client::disconnect()
{
    if (!m_connected) return;
    if (m_socket != uintptr_t(-1)) {
        cli_net_close(cli_to_sock(m_socket));
        m_socket = uintptr_t(-1);
        cli_net_quit();
    }
    m_local_player = NULL_ENTITY;
    m_connected    = false;
}

World& Client::world()
{
    return *m_world;
}

EntityManager& Client::entities()
{
    return *m_entities;
}

void Client::tick(double dt)
{
    if (!m_connected) return;
    process_incoming();
    (void)dt;
}

void Client::send_input(const struct InputSnapshot& snap)
{
    sock_t fd = cli_to_sock(m_socket);
    if (fd == k_bad_sock || !m_connected) return;

    struct WireInput { float dx, dy, dz; float yaw; uint8_t sprint, grab; };
    WireInput wi;
    wi.dx     = snap.wish_dir.x;
    wi.dy     = snap.wish_dir.y;
    wi.dz     = snap.wish_dir.z;
    wi.yaw    = snap.yaw;
    wi.sprint = snap.sprint ? 1 : 0;
    wi.grab   = 0;

    sockaddr_in srv{};
    srv.sin_family      = AF_INET;
    srv.sin_addr.s_addr = m_server_ip;
    srv.sin_port        = htons(m_server_port);
    cli_send_packet(fd, srv, PacketType::InputState, &wi, sizeof(wi));
}

void Client::interpolate(double alpha)
{
    if (!m_connected) return;
    m_entities->each<TransformComponent>([alpha](EntityID, TransformComponent& tr) {
        // Lerp between the previous and latest server-authoritative positions.
        // Write the result into tr.pos (visual render position) without touching
        // prev_pos or net_pos, so on_entity_state always has clean server data.
        tr.pos = glm::mix(tr.prev_pos, tr.net_pos, static_cast<float>(alpha));
    });
}

void Client::send_chat(const std::string& msg)
{
    m_chat_log.push_back("[You] " + msg);
    sock_t fd = cli_to_sock(m_socket);
    if (fd == k_bad_sock) return;
    sockaddr_in srv{};
    srv.sin_family      = AF_INET;
    srv.sin_addr.s_addr = m_server_ip;
    srv.sin_port        = htons(m_server_port);
    cli_send_packet(fd, srv, PacketType::ChatSend, msg.data(), msg.size());
}

void Client::send_appearance_update()
{
    sock_t fd = cli_to_sock(m_socket);
    if (fd == k_bad_sock) return;
    sockaddr_in srv{};
    srv.sin_family      = AF_INET;
    srv.sin_addr.s_addr = m_server_ip;
    srv.sin_port        = htons(m_server_port);

    std::vector<uint8_t> payload;
    payload.push_back(m_appearance.eye_r);
    payload.push_back(m_appearance.eye_g);
    payload.push_back(m_appearance.eye_b);
    uint8_t hlen = (uint8_t)std::min(m_appearance.hair_file.size(), (size_t)255);
    payload.push_back(hlen);
    payload.insert(payload.end(),
        m_appearance.hair_file.begin(),
        m_appearance.hair_file.begin() + hlen);
    payload.push_back(m_appearance.hair_r);
    payload.push_back(m_appearance.hair_g);
    payload.push_back(m_appearance.hair_b);

    // Append slot data
    payload.push_back((uint8_t)std::min(m_equipped_slots.size(), (size_t)255));
    for (auto& [sid, iid] : m_equipped_slots) {
        payload.push_back((uint8_t)sid.size());
        payload.insert(payload.end(), sid.begin(), sid.end());
        payload.push_back((uint8_t)iid.size());
        payload.insert(payload.end(), iid.begin(), iid.end());
    }

    cli_send_packet(fd, srv, PacketType::AppearanceUpdate,
                   payload.data(), payload.size());
}

void Client::process_incoming()
{
    sock_t fd = cli_to_sock(m_socket);
    if (fd == k_bad_sock) return;

    static uint8_t buf[65536];
    for (;;) {
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof(from);
#else
        socklen_t from_len = sizeof(from);
#endif
        int n = ::recvfrom(fd, reinterpret_cast<char*>(buf), (int)sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 6) { break; }

        uint16_t type_raw; uint32_t payload_len;
        std::memcpy(&type_raw,    buf,     2);
        std::memcpy(&payload_len, buf + 2, 4);
        auto type = static_cast<PacketType>(type_raw);
        if (static_cast<int>(n) < 6 + (int)payload_len) continue;
        const uint8_t* payload = buf + 6;

        switch (type) {
        case PacketType::ChunkData:       on_chunk_data       (payload, payload_len); break;
        case PacketType::EntityState:     on_entity_state     (payload, payload_len); break;
        case PacketType::EntitySpawn:     on_entity_spawn     (payload, payload_len); break;
        case PacketType::ChatMessage:     on_chat_message     (payload, payload_len); break;
        case PacketType::AppearanceState: on_appearance_state (payload, payload_len); break;
        default: break;
        }
    }
}

void Client::on_chunk_data(const void* data, size_t len)
{
    struct ChunkHdr { int32_t cx, cy, cz; };
    constexpr size_t k_hdr  = sizeof(ChunkHdr);
    constexpr size_t k_body = static_cast<size_t>(CHUNK_VOL) * sizeof(uint16_t);
    if (!data || len < k_hdr + k_body) return;

    const auto* hdr  = reinterpret_cast<const ChunkHdr*>(data);
    const auto* voxs = reinterpret_cast<const uint16_t*>(
                           static_cast<const uint8_t*>(data) + k_hdr);

    const glm::ivec3 cp{hdr->cx, hdr->cy, hdr->cz};
    Chunk* chunk = m_world->get_or_create_chunk(cp);
    for (int z = 0; z < CHUNK_SIZE; ++z)
        for (int y = 0; y < CHUNK_SIZE; ++y)
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                Voxel v{};
                v.type_id = voxs[x + CHUNK_SIZE * (y + CHUNK_SIZE * z)];
                if (v.type_id != 0 && m_voxel_registry) {
                    const VoxelTypeDef* def = m_voxel_registry->get(v.type_id);
                    if (def) v.flags = def->default_flags;
                }
                chunk->set(x, y, z, v);
            }
    chunk->mark_dirty();
}

void Client::on_entity_state(const void* data, size_t len)
{
    struct EntityStateEntry { uint32_t id; float x, y, z, yaw; };
    constexpr size_t k_entry = sizeof(EntityStateEntry);
    if (!data || len < k_entry) return;

    const size_t count   = len / k_entry;
    const auto*  entries = reinterpret_cast<const EntityStateEntry*>(data);
    for (size_t i = 0; i < count; ++i) {
        EntityID eid = static_cast<EntityID>(entries[i].id);
        auto* tr = m_entities->get_component<TransformComponent>(eid);
        if (!tr) {
            m_entities->adopt(eid);
            TransformComponent new_tr{};
            new_tr.pos     = new_tr.prev_pos = new_tr.net_pos =
                {entries[i].x, entries[i].y, entries[i].z};
            new_tr.yaw = entries[i].yaw;
            m_entities->add_component<TransformComponent>(eid, new_tr);
        } else {
            // Shift the previous server position down; record new server pos.
            // Do NOT use tr->pos here — it holds the blended visual position
            // written by interpolate() and would corrupt the lerp baseline.
            tr->prev_pos = tr->net_pos;
            tr->net_pos  = {entries[i].x, entries[i].y, entries[i].z};
            tr->yaw      = entries[i].yaw;
        }
    }
}

void Client::on_entity_spawn(const void* data, size_t len)
{
    // Packet format (matches server send_entity_spawn_to):
    // [entity_id:4][type:1][type-specific data...][x:4][y:4][z:4][yaw:4]
    // type: 0=player_mob (HumanAppearance), 1=npc_mob (MobComponent), 2=item (WorldItemComponent)

    if (!data || len < 5) return;  // minimum: entity_id(4) + type(1)

    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t eid_raw;
    std::memcpy(&eid_raw, p, 4);
    p += 4;

    uint8_t type = p[0];
    p += 1;

    EntityID eid = static_cast<EntityID>(eid_raw);

    // Adopt entity if not already known
    if (!m_entities->alive(eid)) {
        m_entities->adopt(eid);
    }

    // Parse type-specific data
    if (type == 0) {
        // Player mob with HumanAppearance
        uint8_t species_len = p[0]; p += 1;
        std::string species(reinterpret_cast<const char*>(p), species_len);
        p += species_len;

        uint8_t name_len = p[0]; p += 1;
        std::string display_name(reinterpret_cast<const char*>(p), name_len);
        p += name_len;

        // Read tint
        uint8_t tint_r = p[0], tint_g = p[1], tint_b = p[2], tint_a = p[3];
        p += 4;

        // Read eye color and hair (added after tint in the updated protocol)
        uint8_t eye_r = 30, eye_g = 100, eye_b = 190;
        std::string hair_file = "hair_messy";
        uint8_t hair_r = 89, hair_g = 60, hair_b = 30;
        {
            // Peek ahead to see if there is appearance data before the floats.
            // Layout: [eye_r:1][eye_g:1][eye_b:1][hair_len:1][hair:N][hair_r:1][hair_g:1][hair_b:1]
            // We check we have at least 7 bytes + hair_len bytes left before
            // the 4×4 = 16 float bytes at the end.
            const uint8_t* end = static_cast<const uint8_t*>(data) + len;
            if (p + 4 + 3 + 16 <= end) {  // at least: eyeRGB(3) + hairLen(1) + hairRGB(3) + xyzw(16)
                eye_r = p[0]; eye_g = p[1]; eye_b = p[2];
                uint8_t hair_len = p[3];
                if (p + 4 + hair_len + 3 + 16 <= end) {
                    hair_file = std::string(reinterpret_cast<const char*>(p + 4), hair_len);
                    hair_r = p[4 + hair_len + 0];
                    hair_g = p[4 + hair_len + 1];
                    hair_b = p[4 + hair_len + 2];
                    p += 4 + hair_len + 3;
                }
            }
        }

        // Read position
        float x, y, z, yaw;
        std::memcpy(&x, p, 4); p += 4;
        std::memcpy(&y, p, 4); p += 4;
        std::memcpy(&z, p, 4); p += 4;
        std::memcpy(&yaw, p, 4); p += 4;

        // Create components
        TransformComponent tr{};
        tr.pos = tr.prev_pos = tr.net_pos = {x, y, z};
        tr.yaw = yaw;
        m_entities->add_component<TransformComponent>(eid, tr);
        m_entities->add_component<VelocityComponent>(eid);
        m_entities->add_component<CharacterControllerComponent>(eid);
        m_entities->add_component<HealthComponent>(eid);
        m_entities->add_component<DensityComponent>(eid, DensityComponent{true});
        m_entities->add_component<StatusEffectsComponent>(eid);
        m_entities->add_component<MobTypeTag>(eid, MobTypeTag{"/mob/living/carbon/human"});
        m_entities->add_component<NameComponent>(eid, NameComponent{display_name, "A player."});

        // HumanAppearance for rendering — update if already present (local player
        // or a re-broadcast after AppearanceUpdate)
        auto* app_ptr = m_entities->get_component<HumanAppearance>(eid);
        if (app_ptr) {
            // Update skin tint on the bodypart layer
            for (auto& ov : app_ptr->layers) {
                if (ov.kind == HumanOverlayKind::Bodypart)
                    ov.tint = {tint_r, tint_g, tint_b, tint_a};
            }
            // Refresh eye/hair layers (remove old ones and re-add with new colors)
            app_ptr->layers.erase(
                std::remove_if(app_ptr->layers.begin(), app_ptr->layers.end(),
                    [](const HumanOverlay& ov) {
                        return ov.kind == HumanOverlayKind::Clothing &&
                               (ov.sprite_dir == "human/human_eyes" ||
                                ov.sprite_dir == "human/human_face");
                    }),
                app_ptr->layers.end());
            for (const char* eye_pfx : { "eyes_l", "eyes_r" }) {
                HumanOverlay eye{};
                eye.kind       = HumanOverlayKind::Clothing;
                eye.sprite_dir = "human/human_eyes";
                eye.prefix     = eye_pfx;
                eye.tint       = {eye_r, eye_g, eye_b, 255};
                app_ptr->layers.push_back(eye);
            }
            if (!hair_file.empty()) {
                HumanOverlay hair{};
                hair.kind       = HumanOverlayKind::Clothing;
                hair.sprite_dir = "human/human_face";
                hair.prefix     = hair_file;
                hair.tint       = {hair_r, hair_g, hair_b, 255};
                app_ptr->layers.push_back(hair);
            }
            app_ptr->dirty = true;
        } else {
            HumanAppearance app{};
            // Base bodypart layer (skin)
            HumanOverlay base_layer{};
            base_layer.sprite_dir = "bodyparts_greyscale";
            base_layer.prefix     = "human";
            base_layer.gender     = "_m";
            base_layer.tint       = {tint_r, tint_g, tint_b, tint_a};
            app.layers.push_back(base_layer);
            // Eye overlays
            for (const char* eye_pfx : { "eyes_l", "eyes_r" }) {
                HumanOverlay eye{};
                eye.kind       = HumanOverlayKind::Clothing;
                eye.sprite_dir = "human/human_eyes";
                eye.prefix     = eye_pfx;
                eye.tint       = {eye_r, eye_g, eye_b, 255};
                app.layers.push_back(eye);
            }
            // Hair overlay
            if (!hair_file.empty()) {
                HumanOverlay hair{};
                hair.kind       = HumanOverlayKind::Clothing;
                hair.sprite_dir = "human/human_face";
                hair.prefix     = hair_file;
                hair.tint       = {hair_r, hair_g, hair_b, 255};
                app.layers.push_back(hair);
            }
            app.dirty = true;
            m_entities->add_component<HumanAppearance>(eid, app);
        }

        SDL_Log("Client: spawned player mob entity %u '%s' at (%.1f,%.1f,%.1f)",
                eid, display_name.c_str(), x, y, z);

    } else if (type == 1) {
        // NPC mob with MobComponent
        uint8_t species_len = p[0]; p += 1;
        std::string species(reinterpret_cast<const char*>(p), species_len);
        p += species_len;

        uint8_t name_len = p[0]; p += 1;
        std::string display_name(reinterpret_cast<const char*>(p), name_len);
        p += name_len;

        // Read position
        float x, y, z, yaw;
        std::memcpy(&x, p, 4); p += 4;
        std::memcpy(&y, p, 4); p += 4;
        std::memcpy(&z, p, 4); p += 4;
        std::memcpy(&yaw, p, 4); p += 4;

        // Create components
        TransformComponent tr{};
        tr.pos = tr.prev_pos = tr.net_pos = {x, y, z};
        tr.yaw = yaw;
        m_entities->add_component<TransformComponent>(eid, tr);
        m_entities->add_component<VelocityComponent>(eid);
        m_entities->add_component<CharacterControllerComponent>(eid);
        m_entities->add_component<HealthComponent>(eid);
        m_entities->add_component<DensityComponent>(eid, DensityComponent{true});
        m_entities->add_component<StatusEffectsComponent>(eid);
        m_entities->add_component<MobTypeTag>(eid, MobTypeTag{"/mob/living/carbon/" + species});
        m_entities->add_component<NameComponent>(eid, NameComponent{display_name, "An NPC."});

        // MobComponent for rendering
        MobComponent mob{};
        mob.species = species;
        mob.variant = "default";
        m_entities->add_component<MobComponent>(eid, mob);

        SDL_Log("Client: spawned NPC mob entity %u '%s' (species: %s) at (%.1f,%.1f,%.1f)",
                eid, display_name.c_str(), species.c_str(), x, y, z);

    } else if (type == 2) {
        // World item
        uint8_t id_len = p[0]; p += 1;
        std::string item_id(reinterpret_cast<const char*>(p), id_len);
        p += id_len;

        // Read position
        float x, y, z, yaw;
        std::memcpy(&x, p, 4); p += 4;
        std::memcpy(&y, p, 4); p += 4;
        std::memcpy(&z, p, 4); p += 4;
        std::memcpy(&yaw, p, 4); p += 4;

        // Create components
        TransformComponent tr{};
        tr.pos = tr.prev_pos = tr.net_pos = {x, y, z};
        tr.yaw = yaw;
        m_entities->add_component<TransformComponent>(eid, tr);

        // WorldItemComponent for rendering
        WorldItemComponent item{};
        // Default to resting on ground (facing up) so items render flat, not as billboards
        item.is_resting = true;
        item.rest_face = FaceDir::PosY;
        // Resolve ItemDef from the item registry
        if (m_item_registry) {
            const ItemDef* def = m_item_registry->get(item_id);
            if (def) {
                item.item.def = def;
                item.item.count = 1;
                item.item.integrity = 1.f;
            } else {
                SDL_Log("Client: item '%s' not found in registry", item_id.c_str());
            }
        }
        m_entities->add_component<WorldItemComponent>(eid, item);

        SDL_Log("Client: spawned item entity %u (id: %s) at (%.1f,%.1f,%.1f)",
                eid, item_id.c_str(), x, y, z);
    }
}

void Client::on_chat_message(const void* data, size_t len)
{
    if (!data || len == 0) return;
    m_chat_log.emplace_back(reinterpret_cast<const char*>(data), len);
}

void Client::on_appearance_state(const void* data, size_t len)
{
    // Format: [entity_id:4][eye_r:1][eye_g:1][eye_b:1]
    //         [hair_len:1][hair:N][hair_r:1][hair_g:1][hair_b:1]
    //         [slot_count:1]([slot_id_len:1][slot_id:N][item_id_len:1][item_id:N])*
    if (!data || len < 8) return;
    const uint8_t* p   = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + len;

    uint32_t eid_raw;
    std::memcpy(&eid_raw, p, 4); p += 4;
    EntityID eid = static_cast<EntityID>(eid_raw);

    // Skip update for local player — main.cpp manages their own layers.
    if (eid == m_local_player) return;

    auto* app = m_entities->get_component<HumanAppearance>(eid);
    if (!app) return;

    if (p + 3 > end) return;
    uint8_t eye_r = p[0], eye_g = p[1], eye_b = p[2]; p += 3;

    if (p >= end) return;
    uint8_t hlen = *p++; p += 0; // already incremented
    if (p + hlen + 3 > end) return;
    std::string hair_file(reinterpret_cast<const char*>(p), hlen); p += hlen;
    uint8_t hair_r = p[0], hair_g = p[1], hair_b = p[2]; p += 3;

    // Remove all non-Bodypart layers and rebuild from received data
    std::vector<HumanOverlay> new_layers;
    new_layers.reserve(app->layers.size() + 12);
    for (const auto& ov : app->layers)
        if (ov.kind == HumanOverlayKind::Bodypart)
            new_layers.push_back(ov);

    // Eye overlays
    for (const char* eye_pfx : { "eyes_l", "eyes_r" }) {
        HumanOverlay ov;
        ov.kind       = HumanOverlayKind::Clothing;
        ov.sprite_dir = "human/human_eyes";
        ov.prefix     = eye_pfx;
        ov.tint       = {eye_r, eye_g, eye_b, 255};
        new_layers.push_back(ov);
    }

    // Hair overlay
    if (!hair_file.empty()) {
        HumanOverlay ov;
        ov.kind       = HumanOverlayKind::Clothing;
        ov.sprite_dir = "human/human_face";
        ov.prefix     = hair_file;
        ov.tint       = {hair_r, hair_g, hair_b, 255};
        new_layers.push_back(ov);
    }

    // Slot-based clothing + inhand overlays
    if (p < end) {
        uint8_t slot_count = *p++;
        for (int i = 0; i < slot_count && p < end; ++i) {
            if (p >= end) break;
            uint8_t slen = *p++;
            if (p + slen > end) break;
            std::string slot_id(reinterpret_cast<const char*>(p), slen); p += slen;
            if (p >= end) break;
            uint8_t ilen = *p++;
            if (p + ilen > end) break;
            std::string item_id(reinterpret_cast<const char*>(p), ilen); p += ilen;
            if (item_id.empty()) continue;

            // Check if it's a clothing slot or an inhand slot
            bool is_clothing = false;
            for (const char* cslot : k_clothing_slot_ids) {
                if (slot_id == cslot) { is_clothing = true; break; }
            }
            if (is_clothing) {
                for (const auto& ce : k_clothing_map) {
                    if (item_id == ce.id) {
                        HumanOverlay ov;
                        ov.kind       = HumanOverlayKind::Clothing;
                        ov.sprite_dir = ce.dir;
                        ov.prefix     = ce.name;
                        new_layers.push_back(ov);
                        break;
                    }
                }
            } else {
                // l_hand or r_hand
                bool right = (slot_id == "r_hand");
                for (const auto& ie : k_inhand_map) {
                    if (item_id == ie.id) {
                        HumanOverlay ov;
                        ov.kind       = HumanOverlayKind::Inhand;
                        ov.sprite_dir = std::string("inhands/")
                                      + ie.category
                                      + (right ? "_righthand" : "_lefthand");
                        ov.prefix     = ie.name;
                        new_layers.push_back(ov);
                        break;
                    }
                }
            }
        }
    }

    app->layers = std::move(new_layers);
    app->dirty  = true;
}
