#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <GL/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "formats/a3d.h"
#include "formats/pcam.h"
#include "formats/peft.h"
#include "formats/pliv.h"
#include "formats/pmod.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"HeavyGunnerOpenGLWindow";
constexpr wchar_t kWindowTitle[] = L"Heavy Gunner 3D - OpenGL Port";
constexpr float kLogicalWidth = 480.0f;
constexpr float kLogicalHeight = 320.0f;

struct TextureEntry {
    const wchar_t* file;
    const wchar_t* label;
};

struct ScenePart {
    const wchar_t* meshFile;
    const wchar_t* textureFile;
    pact::Mesh mesh;
    GLuint texture{};
    float uvScaleU{1.0F};
    float uvScaleV{1.0F};
};

struct EnemyVertexAnimation {
    pact::ActionDescriptor action;
    pact::BoneSection boneSection;
    pact::VertexAnimationSection section;
    std::vector<std::vector<pact::Vec3>> boneFrames;
    bool boneLoaded{};
    bool loaded{};
};

struct BoneTransform {
    std::array<float, 4> rotation{{0.0F, 0.0F, 0.0F, 1.0F}};
    pact::Vec3 position{};
};

std::array<float, 4> multiplyQuaternion(const std::array<float, 4>& left,
                                        const std::array<float, 4>& right) {
    const float lx = left[0], ly = left[1], lz = left[2], lw = left[3];
    const float rx = right[0], ry = right[1], rz = right[2], rw = right[3];
    std::array<float, 4> value{{
        lw * rx + lx * rw + ly * rz - lz * ry,
        lw * ry - lx * rz + ly * rw + lz * rx,
        lw * rz + lx * ry - ly * rx + lz * rw,
        lw * rw - lx * rx - ly * ry - lz * rz,
    }};
    const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] +
                                   value[2] * value[2] + value[3] * value[3]);
    if (length > 0.0F) {
        for (float& component : value) component /= length;
    } else {
        value = {{0.0F, 0.0F, 0.0F, 1.0F}};
    }
    return value;
}

pact::Vec3 rotateByQuaternion(const std::array<float, 4>& rotation,
                              const pact::Vec3& value) {
    const pact::Vec3 twiceCross{
        2.0F * (rotation[1] * value.z - rotation[2] * value.y),
        2.0F * (rotation[2] * value.x - rotation[0] * value.z),
        2.0F * (rotation[0] * value.y - rotation[1] * value.x),
    };
    return {
        value.x + rotation[3] * twiceCross.x +
            (rotation[1] * twiceCross.z - rotation[2] * twiceCross.y),
        value.y + rotation[3] * twiceCross.y +
            (rotation[2] * twiceCross.x - rotation[0] * twiceCross.z),
        value.z + rotation[3] * twiceCross.z +
            (rotation[0] * twiceCross.y - rotation[1] * twiceCross.x),
    };
}

BoneTransform combineBoneTransform(const BoneTransform& parent,
                                   const BoneTransform& local) {
    const pact::Vec3 translated = rotateByQuaternion(parent.rotation, local.position);
    return {
        multiplyQuaternion(parent.rotation, local.rotation),
        {parent.position.x + translated.x,
         parent.position.y + translated.y,
         parent.position.z + translated.z},
    };
}

bool buildEnemyBoneFrames(const ScenePart& part, EnemyVertexAnimation& animation,
                          std::string& error) {
    if (animation.boneSection.motions.empty()) {
        error = "PLIV bone section has no decoded motions";
        return false;
    }
    const auto& boneMotion = animation.boneSection.motions.front();
    const auto& descriptor = animation.action.motions[boneMotion.motionIndex];
    const auto& bones = animation.boneSection.header.bones;
    if (boneMotion.tracks.size() != bones.size()) {
        error = "PLIV bone track count does not match its hierarchy";
        return false;
    }
    std::size_t ownedVertices = 0U;
    for (const auto& bone : bones) ownedVertices += bone.rawField;
    if (ownedVertices != part.mesh.positions.size()) {
        error = "PLIV bone vertex ranges do not cover the associated PMOD";
        return false;
    }

    animation.boneFrames.assign(descriptor.frameCount, part.mesh.positions);
    std::vector<BoneTransform> globals(bones.size());
    for (std::size_t frame = 0; frame < descriptor.frameCount; ++frame) {
        std::size_t vertexStart = 0U;
        for (std::size_t boneIndex = 0; boneIndex < bones.size(); ++boneIndex) {
            const auto& bone = bones[boneIndex];
            const auto& track = boneMotion.tracks[boneIndex];
            BoneTransform local;
            if (!track.rotationKeys.empty()) {
                const std::size_t key = track.frameToRotationKey[frame];
                local.rotation = track.rotationKeys[key].value;
            }
            if (!track.positionKeys.empty()) {
                const std::size_t key = track.frameToPositionKey[frame];
                const auto& fixed = track.positionKeys[key].value;
                local.position = {
                    static_cast<float>(fixed[0]) / 65536.0F,
                    static_cast<float>(fixed[1]) / 65536.0F,
                    static_cast<float>(fixed[2]) / 65536.0F,
                };
            }

            const std::uint8_t parentIdentifier = bone.identifiers[1];
            if (parentIdentifier == 0xFFU) {
                globals[boneIndex] = local;
            } else {
                std::size_t parentIndex = bones.size();
                for (std::size_t candidate = 0; candidate < boneIndex; ++candidate) {
                    if (bones[candidate].identifiers[0] == parentIdentifier) {
                        parentIndex = candidate;
                        break;
                    }
                }
                if (parentIndex == bones.size()) {
                    error = "PLIV bone hierarchy references a missing parent";
                    return false;
                }
                globals[boneIndex] = combineBoneTransform(globals[parentIndex], local);
            }

            const std::size_t vertexEnd = vertexStart + bone.rawField;
            for (std::size_t vertex = vertexStart; vertex < vertexEnd; ++vertex) {
                const pact::Vec3 rotated =
                    rotateByQuaternion(globals[boneIndex].rotation,
                                       part.mesh.positions[vertex]);
                animation.boneFrames[frame][vertex] = {
                    rotated.x + globals[boneIndex].position.x,
                    rotated.y + globals[boneIndex].position.y,
                    rotated.z + globals[boneIndex].position.z,
                };
            }
            vertexStart = vertexEnd;
        }
    }
    animation.boneLoaded = true;
    return true;
}

enum class WeaponBehavior {
    Direct,
    Splash,
    Pulse,
    Shield,
    Chain,
};

struct WeaponSpec {
    const char* label;
    const wchar_t* shotLeft;
    const wchar_t* shotRight;
    int maxAmmo;
    ULONGLONG cadenceMs;
    int damage;
    WeaponBehavior behavior;
};

enum class MissionObjective {
    DestroyTargets,
    Survive,
    Boss,
};

struct MissionSpec {
    std::wstring title;
    MissionObjective objective;
    int durationSeconds;
    int targetCount;
    std::size_t enemyModel;
    int enemyHitPoints;
    int enemyTypeId;
    std::size_t patternId;
    std::size_t maxActiveEnemies;
};

const std::array<MissionSpec, 5> kMissionSpecs{{
    {L"OVERTURE", MissionObjective::DestroyTargets, 300, 80, 0U, 3, 1, 1U, 17U},
    {L"VACUUM", MissionObjective::Survive, 120, 0, 0U, 3, 1, 8U, 15U},
    {L"HEAT", MissionObjective::Survive, 110, 0, 1U, 4, 4, 16U, 14U},
    {L"ELEVATION", MissionObjective::DestroyTargets, 300, 300, 1U, 4, 4, 23U, 15U},
    {L"ZERO GRAVITY", MissionObjective::Survive, 140, 0, 2U, 5, 7, 30U, 14U},
}};
const MissionSpec kChallengeMission{
    L"SURVIVAL", MissionObjective::Survive, 80, 0, 0U, 3, 1, 47U, 14U};

constexpr std::array<TextureEntry, 7> kTextures{{
    {L"logo.png", L"Logo screen"},
    {L"loading-en.png", L"Loading atlas"},
    {L"title-ui.png", L"Title UI texture"},
    {L"title-overlay-en.png", L"English title overlay"},
    {L"title-player.png", L"Title player texture"},
    {L"title-ground.png", L"Title ground texture"},
    {L"title-sky.png", L"Title sky texture"},
}};

ComPtr<IWICImagingFactory> g_wicFactory;
HDC g_deviceContext = nullptr;
HGLRC g_glContext = nullptr;
GLuint g_texture = 0;
int g_textureWidth = 1;
int g_textureHeight = 1;
int g_clientWidth = 960;
int g_clientHeight = 640;
std::size_t g_textureIndex = 0;
std::wstring g_status = L"OpenGL ES 1.x compatibility path initialized";
std::array<ScenePart, 3> g_titleScene{{
    {L"title-sky.pmod", L"title-sky.png", {}, 0},
    {L"title-ground.pmod", L"title-ground.png", {}, 0},
    {L"title-player.pmod", L"title-player.png", {}, 0},
}};
std::array<ScenePart, 8> g_missionScene{{
    {L"mission-skybox_001.pmod", L"mission-skybox_001.png", {}, 0},
    {L"mission-ground_001.pmod", L"mission-ground_001.png", {}, 0},
    {L"mission-buildfar_001.pmod", L"mission-buildfar_001.png", {}, 0},
    {L"mission-buildnear_001.pmod", L"mission-buildnear_001.png", {}, 0},
    {L"mission-build01_001.pmod", L"mission-build01_001.png", {}, 0},
    {L"mission-build02_001.pmod", L"mission-build02_001.png", {}, 0},
    {L"mission-build03_001.pmod", L"mission-build03_001.png", {}, 0},
    {L"mission-build04_001.pmod", L"mission-build04_001.png", {}, 0},
}};
std::array<ScenePart, 5> g_missionScene2{{
    {L"stage2-skybox.pmod", L"stage2-skybox.png", {}, 0},
    {L"stage2-ground.pmod", L"stage2-ground.png", {}, 0},
    {L"stage2-farbuild.pmod", L"stage2-farbuild.png", {}, 0},
    {L"stage2-battery.pmod", L"stage2-battery.png", {}, 0},
    {L"stage2-spaceship.pmod", L"stage2-spaceship.png", {}, 0},
}};
std::array<ScenePart, 3> g_missionScene3{{
    {L"stage3-skybox.pmod", L"stage3-skybox.png", {}, 0},
    {L"stage3-ground.pmod", L"stage3-ground.png", {}, 0},
    {L"stage3-fire.pmod", L"stage3-fire.png", {}, 0},
}};
std::array<ScenePart, 3> g_missionScene4{{
    {L"stage4-planet.pmod", L"stage4-planet.png", {}, 0},
    {L"stage4-mainwall.pmod", L"stage4-planet.png", {}, 0},
    {L"stage4-movewall.pmod", L"stage4-planet.png", {}, 0},
}};
std::array<ScenePart, 6> g_missionScene5{{
    {L"stage5-space.pmod", L"stage5-space.png", {}, 0},
    {L"stage5-ground.pmod", L"stage5-ground.png", {}, 0},
    {L"stage5-gate.pmod", L"stage5-gate.png", {}, 0},
    {L"stage5-thunder.pmod", L"stage5-thunder.png", {}, 0},
    {L"stage5-quartz01.pmod", L"stage5-quartz.png", {}, 0},
    {L"stage5-quartz02.pmod", L"stage5-quartz.png", {}, 0},
}};
std::array<ScenePart, 7> g_enemySceneParts{{
    {L"enemy-zako01.pmod", L"enemy-zako01.png", {}, 0},
    {L"enemy-zako02.pmod", L"enemy-zako02.png", {}, 0},
    {L"enemy-zako03.pmod", L"enemy-zako03.png", {}, 0},
    {L"enemy-middle02.pmod", L"enemy-middle02.png", {}, 0},
    {L"enemy-bmissile.pmod", L"enemy-bmissile.png", {}, 0},
    {L"enemy-middle01.pmod", L"enemy-middle01.png", {}, 0},
    {L"enemy-boss01.pmod", L"enemy-boss01.png", {}, 0},
}};
constexpr std::array<float, 7> kEnemyModelScales{{
    1.0F, 1.0F, 1.0F, 0.88F, 0.92F, 0.82F, 2.20F,
}};
std::array<GLuint, 4> g_hitEffectTextures{};
GLuint g_enemyDieTexture = 0;
GLuint g_playerHitTexture = 0;
GLuint g_playerShieldHitTexture = 0;
GLuint g_missileExplosionTexture = 0;
std::array<GLuint, 2> g_empFieldTextures{};
GLuint g_shieldFieldTexture = 0;
GLuint g_thunderFireTexture = 0;
GLuint g_bossLaserTexture = 0;

enum class CombatEffect {
    None,
    Hit,
    EnemyDie,
    MissileExplosion,
    Emp,
    Shield,
    Thunder,
};

CombatEffect g_combatEffect = CombatEffect::None;
ULONGLONG g_combatEffectStart = 0;
ULONGLONG g_playerHitEffectStart = 0;
float g_combatEffectX = kLogicalWidth * 0.5F;
float g_combatEffectY = kLogicalHeight * 0.5F;
std::size_t g_hitEffectVariant = 0U;
std::array<pact::EffectDescriptor, 7> g_combatEffectDescriptors{};
std::array<bool, 7> g_combatEffectDescriptorLoaded{};
pact::EffectDescriptor g_bossLaserEffectDescriptor;
bool g_bossLaserEffectDescriptorLoaded = false;

const pact::EffectMotionDescriptor* combatEffectMotion(CombatEffect effect) {
    const auto index = static_cast<std::size_t>(effect);
    if (index >= g_combatEffectDescriptors.size() ||
        !g_combatEffectDescriptorLoaded[index] ||
        g_combatEffectDescriptors[index].motions.empty()) {
        return nullptr;
    }
    return &g_combatEffectDescriptors[index].motions.front();
}

ULONGLONG combatEffectDurationMs(CombatEffect effect) {
    const auto* motion = combatEffectMotion(effect);
    if (motion == nullptr || motion->framesPerSecond == 0U) {
        return 650ULL;
    }
    return (static_cast<ULONGLONG>(motion->frameCount) * 1000ULL +
            motion->framesPerSecond - 1ULL) / motion->framesPerSecond;
}

std::size_t combatEffectSpriteFrame(CombatEffect effect, ULONGLONG elapsed,
                                    std::size_t spriteFrameCount) {
    const auto* motion = combatEffectMotion(effect);
    if (motion == nullptr || motion->framesPerSecond == 0U ||
        motion->frameCount == 0U || spriteFrameCount == 0U) {
        return 0U;
    }
    const std::size_t sourceFrame = std::min<std::size_t>(
        static_cast<std::size_t>(elapsed * motion->framesPerSecond / 1000ULL),
        static_cast<std::size_t>(motion->frameCount - 1U));
    return std::min<std::size_t>(
        sourceFrame * spriteFrameCount / motion->frameCount,
        spriteFrameCount - 1U);
}
std::array<ScenePart, 8> g_weaponSceneParts{{
    {L"player-lmg.pmod", L"player-lmg.png", {}, 0},
    {L"player-shower.pmod", L"player-shower.png", {}, 0},
    {L"player-chaser.pmod", L"player-chaser.png", {}, 0},
    {L"player-missile.pmod", L"player-missile.png", {}, 0},
    {L"player-emp.pmod", L"player-emp.png", {}, 0},
    {L"player-hmg.pmod", L"player-hmg.png", {}, 0},
    {L"player-shield.pmod", L"player-shield.png", {}, 0},
    {L"player-thunder.pmod", L"player-thunder.png", {}, 0},
}};
constexpr std::array<WeaponSpec, 8> kWeaponSpecs{{
    {"CRUSADER LMG", L"lmg-shot-l.wav", L"lmg-shot-r.wav",
        500, 90ULL, 1, WeaponBehavior::Direct},
    {"PLASMA SHOWER", L"shower-shot-l.wav", L"shower-shot-r.wav",
        160, 220ULL, 2, WeaponBehavior::Direct},
    {"HOMING CHASER", L"chaser-shot-l.wav", L"chaser-shot-r.wav",
        80, 450ULL, 4, WeaponBehavior::Direct},
    {"AM MISSILE", L"missile-shot-l.wav", L"missile-shot-r.wav",
        40, 700ULL, 6, WeaponBehavior::Splash},
    {"EMP SHOCKWAVE", L"emp-shot.wav", L"emp-shot.wav",
        12, 1800ULL, 1, WeaponBehavior::Pulse},
    {"AVENGER HMG", L"hmg-shot.wav", L"hmg-shot.wav",
        300, 125ULL, 2, WeaponBehavior::Direct},
    {"SHIELD RECHARGER", nullptr, nullptr,
        8, 1600ULL, 0, WeaponBehavior::Shield},
    {"THUNDERBOLT", L"thunder-shot.wav", L"thunder-shot.wav",
        60, 320ULL, 4, WeaponBehavior::Chain},
}};
constexpr std::array<int, 9> kArmorySlotToWeapon{{
    0, 4, 3,
    5, 1, 7,
    2, -1, 6,
}};
constexpr std::array<float, 8> kWeaponModelScales{{
    1.0F, 1.30F, 1.35F, 0.78F, 0.82F, 1.05F, 1.35F, 1.0F,
}};
constexpr std::array<int, 8> kWeaponTargetTexture{{
    0, 1, 2, 3, -1, 4, -1, 5,
}};
constexpr std::array<float, 8> kWeaponTargetSizes{{
    40.0F, 72.0F, 72.0F, 44.0F, 0.0F, 44.0F, 0.0F, 44.0F,
}};
std::size_t g_equippedWeapon = 0;
std::size_t g_selectedStageTheme = 0;
std::vector<MissionSpec> g_campaignMissions;
pact::A3dTable g_stageTable;
pact::A3dTable g_enemyTable;
pact::A3dTable g_enemyPatternTable;
bool g_missionSceneLoaded = false;
bool g_mission3dMode = true;
float g_missionYawDegrees = 0.0F;
float g_cameraYawOffsetDegrees = 0.0F;
float g_cameraPitchDegrees = 0.0F;
ULONGLONG g_lastCameraUpdateTime = 0;

struct EnemyInstance {
    pact::Vec3 position;
    float targetX{};
    float targetY{};
    bool alive{true};
    ULONGLONG respawnAt{};
    int hitPoints{3};
};

std::array<EnemyInstance, 18> g_enemies{{
    {{-48.0F, 58.0F, -155.0F}, 138.0F, 142.0F, true},
    {{-23.0F, 72.0F, -128.0F}, 192.0F, 96.0F, true},
    {{4.0F, 52.0F, -105.0F}, 279.0F, 142.0F, true},
    {{35.0F, 66.0F, -142.0F}, 321.0F, 124.0F, true},
    {{58.0F, 44.0F, -188.0F}, 343.0F, 153.0F, true},
    {{-70.0F, 38.0F, -210.0F}, 136.0F, 194.0F, true},
    {{-10.0F, 91.0F, -220.0F}, 228.0F, 130.0F, true},
    {{72.0F, 82.0F, -235.0F}, 350.0F, 197.0F, true},
    {{18.0F, 105.0F, -260.0F}, 281.0F, 118.0F, true},
    {{-92.0F, 96.0F, -275.0F}, 105.0F, 112.0F, true},
    {{-38.0F, 102.0F, -180.0F}, 188.0F, 76.0F, true},
    {{-18.0F, 120.0F, -200.0F}, 217.0F, 62.0F, true},
    {{2.0F, 132.0F, -225.0F}, 240.0F, 55.0F, true},
    {{22.0F, 115.0F, -200.0F}, 267.0F, 68.0F, true},
    {{44.0F, 102.0F, -190.0F}, 294.0F, 78.0F, true},
    {{-55.0F, 78.0F, -235.0F}, 165.0F, 100.0F, true},
    {{55.0F, 82.0F, -245.0F}, 315.0F, 105.0F, true},
    {{0.0F, 150.0F, -270.0F}, 240.0F, 45.0F, true},
}};
pact::Vec3 g_sceneMinimum{};
pact::Vec3 g_sceneMaximum{};
std::size_t g_sceneVertices = 0;
std::size_t g_sceneTriangles = 0;
pact::CameraTrack g_titleCamera;
ULONGLONG g_cameraStartTime = 0;
bool g_cameraLoaded = false;
pact::Mesh g_gunFireMesh;
pact::ActionDescriptor g_gunFireAction;
pact::UnflowedTextureSection g_gunFireAnimation;
std::array<EnemyVertexAnimation, 7> g_enemyVertexAnimations;
GLuint g_gunFireTexture = 0;
ULONGLONG g_gunFireStartTime = 0;
bool g_gunFireLoaded = false;
bool g_fireMode = false;
bool g_fireFrameLogged = false;
bool g_fireLoopLogged = false;
std::uint32_t g_fireObservedFrames = 0;
bool g_meshMode = false;
bool g_startInMeshMode = false;
bool g_startInFireMode = false;
bool g_meshFrameLogged = false;
bool g_captureRequested = false;
bool g_captureWritten = false;
bool g_captureAttackFrame = false;

enum class BgmTrack {
    None,
    Title,
    Mission1Head,
    Mission1Body,
    Mission2,
    Mission3Head,
    Mission3Body,
    Mission4,
    Mission5,
};
BgmTrack g_bgmTrack = BgmTrack::None;
std::uint32_t g_bgmLoopCount = 0;

enum class RuntimeState {
    Title,
    MainMenu,
    Challenge,
    Extra,
    Settings,
    StageSelect,
    Story,
    Armory,
    ArmoryConfirm,
    Playing,
    Paused,
};

RuntimeState g_runtimeState = RuntimeState::Title;
RuntimeState g_startRuntimeState = RuntimeState::Title;
bool g_gameFlow = false;
bool g_startInGameMode = false;
bool g_startWithShot = false;
bool g_startCompleted = false;
bool g_startFailed = false;
int g_startWeaponOverride = -1;
GLuint g_titleUiTexture = 0;
GLuint g_menuUiTexture = 0;
std::array<GLuint, 5> g_storyTextures{};
GLuint g_armoryTexture = 0;
GLuint g_armoryConfirmTexture = 0;
GLuint g_armoryUi1Texture = 0;
GLuint g_armoryUi2Texture = 0;
GLuint g_armoryUi3Texture = 0;
GLuint g_popupEnTexture = 0;
GLuint g_gameoverTexture = 0;
GLuint g_gameplayReferenceTexture = 0;
GLuint g_pausedReferenceTexture = 0;
GLuint g_resultsTexture = 0;
GLuint g_cockpitTexture = 0;
GLuint g_pauseTexture = 0;
std::array<GLuint, 6> g_targetTextures{};
float g_aimX = kLogicalWidth * 0.5F;
float g_aimY = kLogicalHeight * 0.5F;
bool g_shotActive = false;
ULONGLONG g_shotStartTime = 0;
std::uint32_t g_shotCount = 0;
bool g_leftTriggerHeld = false;
bool g_rightTriggerHeld = false;
bool g_leftReloading = false;
bool g_rightReloading = false;
ULONGLONG g_leftLastShotTime = 0;
ULONGLONG g_rightLastShotTime = 0;
ULONGLONG g_leftReloadCompleteTime = 0;
ULONGLONG g_rightReloadCompleteTime = 0;
int g_leftAmmo = 500;
int g_rightAmmo = 500;
int g_targetsRemaining = 80;
std::size_t g_menuSelection = 0;
std::size_t g_stageRow = 0;
std::size_t g_stageColumn = 0;
std::size_t g_armorySelection = 0;
bool g_missionComplete = false;
bool g_missionFailed = false;
bool g_settingTilt = true;
bool g_settingMultiTouch = true;
bool g_settingVibration = true;
bool g_settingSound = true;
bool g_settingQuality = true;
bool g_settingReverseY = false;
int g_settingSensitivity = 5;
bool g_challengeMode = false;
int g_playerPoints = 0;
std::array<int, 8> g_weaponLevels{};
std::array<bool, 25> g_clearedMissions{};
ULONGLONG g_missionStartTime = 0;
ULONGLONG g_pauseStartTime = 0;
int g_lastDamageCycle = -1;
float g_playerShield = 1.0F;
float g_playerHull = 1.0F;

const MissionSpec& selectedMissionSpec() {
    if (g_challengeMode) {
        return kChallengeMission;
    }
    const std::size_t campaignIndex = g_selectedStageTheme * 5U + g_stageRow;
    if (campaignIndex < g_campaignMissions.size()) {
        return g_campaignMissions[campaignIndex];
    }
    return kMissionSpecs[std::min<std::size_t>(
        g_selectedStageTheme, kMissionSpecs.size() - 1U)];
}

struct PatternRuntime {
    int movementType{1};
    int attackMode{1};
    float entryDurationSeconds{24.0F};
    float attackWindowSeconds{5.0F};
    float cycleSeconds{24.0F};
};

PatternRuntime selectedPatternRuntime() {
    const MissionSpec& mission = selectedMissionSpec();
    if (mission.patternId >= g_enemyPatternTable.rowCount) {
        return {};
    }
    const auto value = [&](std::size_t field, int fallback) {
        return g_enemyPatternTable.signedValue(mission.patternId, field, fallback);
    };
    PatternRuntime pattern;
    // hg-enemy-pattern.a3d fields consumed by HG_ENEMY_GroupAddSpawn:
    // [1] movement type, [9] attack mode, [16] movement duration. Field [17]
    // is copied to group+0xb4 and spans the per-child attack-start window.
    pattern.movementType = std::clamp(value(1U, 1), 1, 6);
    pattern.attackMode = std::clamp(value(9U, 1), 1, 3);
    pattern.entryDurationSeconds = std::max(
        1.0F, static_cast<float>(value(16U, 24000)) / 1000.0F);
    pattern.attackWindowSeconds = std::max(
        0.0F, static_cast<float>(value(17U, 5000)) / 1000.0F);
    if (mission.patternId == 1U && pattern.movementType == 1) {
        // Original randomized 24,000 ms to 25,931 ms for the captured first group.
        pattern.entryDurationSeconds = 25.931F;
    } else if (mission.patternId == 5U && pattern.movementType == 2) {
        // Captured Hunted group+0x60 duration after the original randomR call.
        pattern.entryDurationSeconds = 24.011F;
    }
    pattern.cycleSeconds = pattern.entryDurationSeconds;
    return pattern;
}

// Recovered from Heavy Gunner 3D 1.0.8's HG_ENEMY_Add/HG_ENEMY_Process
// runtime records for campaign pattern 1. Values are the original Q16.16
// coordinates converted to floats; the angle offsets are original integer degrees.
struct OriginalType1Slot {
    float radius;
    float angleOffsetDegrees;
    float heightOffset;
    float attackStartSeconds;
    float diveDurationSeconds;
};

constexpr std::array<OriginalType1Slot, 18> kOriginalPattern1Slots{{
    {450.00F,   0.0F,  0.00F, 15.7079F, 2.379F},
    {419.00F, -10.0F, 41.52F, 14.8341F, 2.129F},
    {498.56F,   4.0F, 18.65F, 16.6551F, 2.657F},
    {403.04F,  12.0F, 13.62F, 14.3519F, 2.688F},
    {510.56F,   0.0F,  0.00F, 16.8100F, 2.269F},
    {487.58F, -17.0F, 34.79F, 16.4730F, 2.219F},
    {557.52F,  24.0F, 49.86F, 17.5779F, 2.512F},
    {545.43F,  19.0F,  5.96F, 17.3960F, 2.473F},
    {572.42F,   0.0F,  0.00F, 17.8750F, 2.884F},
    {587.35F, -22.0F, 24.37F, 17.9930F, 2.105F},
    {566.00F, -10.0F, 32.86F, 17.6850F, 2.082F},
    {529.71F, -13.0F, 44.35F, 17.1349F, 2.465F},
    {630.71F,   0.0F,  0.00F, 18.6410F, 2.003F},
    {588.41F, -18.0F, 12.38F, 18.0259F, 2.458F},
    {591.31F,   4.0F, 13.46F, 18.0771F, 2.600F},
    {591.28F, -14.0F, 22.72F, 18.0771F, 2.139F},
    {694.09F,   0.0F,  0.00F, 19.2370F, 2.956F},
    {673.11F,   7.0F, 14.76F, 19.0320F, 2.198F},
}};

struct OriginalEnemyMotion {
    pact::Vec3 position;
    pact::Vec3 velocity;
    bool diving{};
    float diveProgress{};
};

pact::Vec3 originalPattern1Position(std::size_t index, float waveSeconds) {
    constexpr float pi = 3.14159265358979323846F;
    constexpr float groupAngleDegrees = 192.0F;
    constexpr float groupStartY = 340.0F;
    constexpr float formationY = 30.0F;
    constexpr float recoveredEntryDuration = 25.931F;
    const OriginalType1Slot& slot = kOriginalPattern1Slots[index];

    const auto entryPosition = [&](float seconds) {
        const float fraction = std::clamp(seconds / recoveredEntryDuration, 0.0F, 1.0F);
        const float angle = (groupAngleDegrees + slot.angleOffsetDegrees) * pi / 180.0F;
        const float remainingRadius = slot.radius * (1.0F - fraction);
        return pact::Vec3{
            std::sin(angle) * remainingRadius,
            groupStartY + (formationY - groupStartY) * fraction + slot.heightOffset,
            std::cos(angle) * remainingRadius,
        };
    };

    if (waveSeconds < slot.attackStartSeconds) {
        return entryPosition(waveSeconds);
    }

    // State 3 in the original copies the current coordinate into the child origin,
    // then approaches (0,30,0). Its timer is capped at duration - 400 ms and
    // the interpolation divisor is duration - 100 ms.
    const pact::Vec3 origin = entryPosition(slot.attackStartSeconds);
    const float elapsed = std::min(
        waveSeconds - slot.attackStartSeconds,
        std::max(0.0F, slot.diveDurationSeconds - 0.400F));
    const float divisor = std::max(0.001F, slot.diveDurationSeconds - 0.100F);
    const float remaining = std::max(0.0F, 1.0F - elapsed / divisor);
    return pact::Vec3{
        origin.x * remaining,
        formationY + (origin.y - formationY) * remaining,
        origin.z * remaining,
    };
}

OriginalEnemyMotion originalPattern1Motion(std::size_t index, float missionSeconds,
                                           const PatternRuntime& pattern) {
    // The original stage continuously adds groups. A fresh group begins when the
    // recovered group movement duration expires.
    const float sourceCycle = std::max(1.0F, pattern.entryDurationSeconds);
    const float waveSeconds = std::fmod(std::max(0.0F, missionSeconds), sourceCycle);
    constexpr float sampleSeconds = 1.0F / 60.0F;
    const pact::Vec3 position = originalPattern1Position(index, waveSeconds);
    const pact::Vec3 previous = originalPattern1Position(
        index, std::max(0.0F, waveSeconds - sampleSeconds));
    const pact::Vec3 next = originalPattern1Position(
        index, std::min(sourceCycle - 0.001F, waveSeconds + sampleSeconds));
    const OriginalType1Slot& slot = kOriginalPattern1Slots[index];
    const float diveElapsed = waveSeconds - slot.attackStartSeconds;
    return {
        position,
        {(next.x - previous.x) / (sampleSeconds * 2.0F),
         (next.y - previous.y) / (sampleSeconds * 2.0F),
         (next.z - previous.z) / (sampleSeconds * 2.0F)},
        diveElapsed >= 0.0F && diveElapsed <= slot.diveDurationSeconds,
        std::clamp(diveElapsed / slot.diveDurationSeconds, 0.0F, 1.0F),
    };
}

// Exact Q16.16 coordinates captured from the original ARM HG_ENEMY_Process
// for enemy pattern 5 (Hunted/Pendulum, movement type 2). The capture times
// come from TAG_HG_ENEMYGROUP+0x5c; positions are TAG_HG_ENEMYINFO
// +0x18/+0x1c/+0x20. The first key for every slot is its original spawn
// coordinate at +0x0c/+0x10/+0x14. Keeping this trace here avoids replacing
// the recovered behavior with a synthetic sine or hand-authored flight path.
struct OriginalType2Slot {
    std::array<pact::Vec3, 5> keyframes;
};

constexpr std::array<float, 5> kOriginalPattern5Times{{
    0.000F, 9.597F, 13.809F, 17.629F, 21.921F,
}};

constexpr std::array<OriginalType2Slot, 16> kOriginalPattern5Slots{{
    {{{{-85.762024F, 212.000000F, -441.753387F}, {5.074158F, 125.707245F, -270.089218F}, {-15.089188F, 88.948853F, -190.604706F}, {-8.796280F, 46.662872F, -64.032562F}, {-8.796280F, 46.662872F, -64.032562F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {72.232376F, 152.637527F, -287.905945F}, {30.986557F, 115.879135F, -207.792038F}, {6.582626F, 59.421661F, -72.536713F}, {6.582626F, 59.421661F, -72.536713F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {-41.888748F, 166.337067F, -239.781342F}, {-40.326981F, 118.989288F, -149.051529F}, {-40.326981F, 118.989288F, -149.051529F}, {-40.326981F, 118.989288F, -149.051529F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {-17.462891F, 172.165878F, -255.430984F}, {-29.954239F, 135.407486F, -178.721710F}, {-10.042542F, 57.659317F, -44.165436F}, {-10.042542F, 57.659317F, -44.165436F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {24.181580F, 175.211349F, -271.080490F}, {-1.772369F, 138.452957F, -192.618912F}, {-4.962906F, 74.312607F, -74.261902F}, {-4.962906F, 74.312607F, -74.261902F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {19.024292F, 130.022369F, -313.553513F}, {30.852005F, 103.561752F, -220.184418F}, {11.278763F, 51.099182F, -60.192307F}, {11.278763F, 51.099182F, -60.192307F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {148.837738F, 152.785446F, -286.335144F}, {120.937973F, 126.324829F, -193.765625F}, {51.669022F, 72.487228F, -74.879211F}, {51.669022F, 72.487228F, -74.879211F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {161.647476F, 172.174149F, -298.003922F}, {130.543655F, 145.713531F, -201.336884F}, {37.403809F, 65.632111F, -52.212036F}, {37.403809F, 65.632111F, -52.212036F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {153.803055F, 184.594009F, -308.785202F}, {125.606079F, 158.133392F, -209.377014F}, {44.986191F, 80.240356F, -67.664032F}, {44.986191F, 80.240356F, -67.664032F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {-39.385437F, 146.625839F, -344.310623F}, {-8.653351F, 120.165222F, -245.134827F}, {1.151260F, 73.347641F, -107.164474F}, {1.151260F, 73.347641F, -107.164474F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {-1.777924F, 129.495087F, -356.321701F}, {-24.625320F, 104.206360F, -250.998169F}, {-16.991226F, 62.535034F, -104.324646F}, {-16.991226F, 62.535034F, -104.324646F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {-67.948273F, 185.846634F, -340.918976F}, {-70.246902F, 160.557907F, -235.800735F}, {-35.619095F, 95.581787F, -97.540543F}, {-35.619095F, 95.581787F, -97.540543F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {-132.830048F, 181.907364F, -294.460999F}, {-112.924057F, 156.618637F, -198.806778F}, {-35.733261F, 72.654007F, -55.576447F}, {-35.733261F, 72.654007F, -55.576447F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {-97.150406F, 165.107956F, -332.618820F}, {-90.279892F, 139.819229F, -228.034958F}, {-42.496872F, 81.977173F, -91.298492F}, {-42.496872F, 81.977173F, -91.298492F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {53.166779F, 175.556183F, -346.517578F}, {14.739502F, 150.267456F, -247.690323F}, {-0.204987F, 80.785797F, -88.962494F}, {-0.204987F, 80.785797F, -88.962494F}}}},
    {{{{-85.762024F, 212.000000F, -441.753387F}, {5.535858F, 131.178009F, -400.845978F}, {-31.168091F, 103.217468F, -282.019562F}, {-36.828217F, 74.825699F, -171.053360F}, {-36.828217F, 74.825699F, -171.053360F}}}},
}};

OriginalEnemyMotion originalPattern5Motion(std::size_t index, float missionSeconds,
                                           const PatternRuntime& pattern) {
    const auto& slot = kOriginalPattern5Slots[index % kOriginalPattern5Slots.size()];
    const float cycleSeconds = std::max(
        kOriginalPattern5Times.back(), pattern.entryDurationSeconds);
    const float waveSeconds = std::fmod(std::max(0.0F, missionSeconds), cycleSeconds);
    std::size_t segment = 0U;
    while (segment + 1U < kOriginalPattern5Times.size() &&
           waveSeconds > kOriginalPattern5Times[segment + 1U]) {
        ++segment;
    }
    if (segment + 1U >= kOriginalPattern5Times.size()) {
        return {slot.keyframes.back(), {}, true, 1.0F};
    }
    const float fromTime = kOriginalPattern5Times[segment];
    const float toTime = kOriginalPattern5Times[segment + 1U];
    const float fraction = std::clamp(
        (waveSeconds - fromTime) / std::max(0.001F, toTime - fromTime),
        0.0F, 1.0F);
    const auto& from = slot.keyframes[segment];
    const auto& to = slot.keyframes[segment + 1U];
    const float inverseDuration = 1.0F / std::max(0.001F, toTime - fromTime);
    return {
        {from.x + (to.x - from.x) * fraction,
         from.y + (to.y - from.y) * fraction,
         from.z + (to.z - from.z) * fraction},
        {(to.x - from.x) * inverseDuration,
         (to.y - from.y) * inverseDuration,
         (to.z - from.z) * inverseDuration},
        segment >= 2U,
        segment >= 2U ? fraction : 0.0F,
    };
}

RuntimeState missionHubState() {
    return g_challengeMode ? RuntimeState::Challenge : RuntimeState::StageSelect;
}

const wchar_t* runtimeStateName() {
    switch (g_runtimeState) {
    case RuntimeState::Title: return L"TITLE";
    case RuntimeState::MainMenu: return L"MAIN_MENU";
    case RuntimeState::Challenge: return L"CHALLENGE";
    case RuntimeState::Extra: return L"EXTRA";
    case RuntimeState::Settings: return L"SETTINGS";
    case RuntimeState::StageSelect: return L"STAGE_SELECT";
    case RuntimeState::Story: return L"STORY";
    case RuntimeState::Armory: return L"ARMORY";
    case RuntimeState::ArmoryConfirm: return L"ARMORY_CONFIRM";
    case RuntimeState::Playing: return L"PLAYING";
    case RuntimeState::Paused: return L"PAUSED";
    }
    return L"UNKNOWN";
}

std::filesystem::path executableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path saveDataPath() {
    return executableDirectory() / L"heavy-gunner-save.ini";
}

int weaponUpgradeCost(std::size_t weapon) {
    const int level = g_weaponLevels[std::min<std::size_t>(weapon, g_weaponLevels.size() - 1U)];
    return level >= 5 ? 0 : 100 * (level + 1);
}

int weaponDamage(std::size_t weapon) {
    const auto index = std::min<std::size_t>(weapon, kWeaponSpecs.size() - 1U);
    const int base = kWeaponSpecs[index].damage;
    return base + (base * g_weaponLevels[index] + 3) / 4;
}

int weaponAmmoCapacity(std::size_t weapon) {
    const auto index = std::min<std::size_t>(weapon, kWeaponSpecs.size() - 1U);
    const int base = kWeaponSpecs[index].maxAmmo;
    return base + (base * g_weaponLevels[index]) / 5;
}

ULONGLONG weaponCadence(std::size_t weapon) {
    const auto index = std::min<std::size_t>(weapon, kWeaponSpecs.size() - 1U);
    const ULONGLONG base = kWeaponSpecs[index].cadenceMs;
    return std::max<ULONGLONG>(40ULL,
        base * static_cast<ULONGLONG>(100 - g_weaponLevels[index] * 5) / 100ULL);
}

ULONGLONG weaponReloadDuration(std::size_t weapon) {
    constexpr std::array<ULONGLONG, 8> baseReloadMs{{
        1100ULL, 1300ULL, 1450ULL, 1800ULL,
        2400ULL, 1200ULL, 2200ULL, 1700ULL,
    }};
    const auto index = std::min<std::size_t>(weapon, baseReloadMs.size() - 1U);
    return std::max<ULONGLONG>(550ULL,
        baseReloadMs[index] *
            static_cast<ULONGLONG>(100 - g_weaponLevels[index] * 6) / 100ULL);
}

void saveProgress() {
    std::ofstream output(saveDataPath(), std::ios::trunc);
    if (!output) {
        return;
    }
    output << "version=1\n"
           << "points=" << g_playerPoints << '\n'
           << "equipped=" << g_equippedWeapon << '\n'
           << "sound=" << static_cast<int>(g_settingSound) << '\n'
           << "quality=" << static_cast<int>(g_settingQuality) << '\n'
           << "reverseY=" << static_cast<int>(g_settingReverseY) << '\n'
           << "sensitivity=" << g_settingSensitivity << '\n'
           << "weaponLevels=";
    for (std::size_t index = 0; index < g_weaponLevels.size(); ++index) {
        if (index != 0U) output << ',';
        output << g_weaponLevels[index];
    }
    output << "\ncleared=";
    for (const bool cleared : g_clearedMissions) {
        output << (cleared ? '1' : '0');
    }
    output << '\n';
}

void loadProgress() {
    std::ifstream input(saveDataPath());
    if (!input) {
        return;
    }
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1U);
        try {
            if (key == "points") {
                g_playerPoints = std::clamp(std::stoi(value), 0, 9999999);
            } else if (key == "equipped") {
                g_equippedWeapon = std::clamp<std::size_t>(
                    static_cast<std::size_t>(std::stoul(value)), 0U,
                    kWeaponSpecs.size() - 1U);
            } else if (key == "sound") {
                g_settingSound = std::stoi(value) != 0;
            } else if (key == "quality") {
                g_settingQuality = std::stoi(value) != 0;
            } else if (key == "reverseY") {
                g_settingReverseY = std::stoi(value) != 0;
            } else if (key == "sensitivity") {
                g_settingSensitivity = std::clamp(std::stoi(value), 1, 10);
            } else if (key == "weaponLevels") {
                std::istringstream values(value);
                std::string item;
                std::size_t index = 0;
                while (index < g_weaponLevels.size() && std::getline(values, item, ',')) {
                    g_weaponLevels[index++] = std::clamp(std::stoi(item), 0, 5);
                }
            } else if (key == "cleared") {
                for (std::size_t index = 0;
                     index < g_clearedMissions.size() && index < value.size(); ++index) {
                    g_clearedMissions[index] = value[index] == '1';
                }
            }
        } catch (...) {
            // Preserve valid fields if a manually edited save contains one bad value.
        }
    }
}

void resetProgress() {
    g_playerPoints = 0;
    g_weaponLevels.fill(0);
    g_clearedMissions.fill(false);
    g_equippedWeapon = 0U;
    g_settingTilt = true;
    g_settingMultiTouch = true;
    g_settingVibration = true;
    g_settingSound = true;
    g_settingQuality = true;
    g_settingReverseY = false;
    g_settingSensitivity = 5;
    saveProgress();
}

void updateWindowTitle(HWND window) {
    const std::wstring title = std::wstring(kWindowTitle) + L"  |  " +
        kTextures[g_textureIndex].label + L"  |  " + g_status;
    SetWindowTextW(window, title.c_str());
}

void tryUpgradeWeapon(HWND window) {
    int& level = g_weaponLevels[g_equippedWeapon];
    if (level >= 5) {
        g_status = L"ARMORY: weapon is already at maximum level";
    } else {
        const int cost = weaponUpgradeCost(g_equippedWeapon);
        if (g_playerPoints < cost) {
            g_status = L"ARMORY: not enough points (need " +
                std::to_wstring(cost) + L" P)";
        } else {
            g_playerPoints -= cost;
            ++level;
            saveProgress();
            g_status = L"ARMORY: upgraded weapon to level " +
                std::to_wstring(level + 1);
        }
    }
    updateWindowTitle(window);
    InvalidateRect(window, nullptr, FALSE);
}

void stopBgm() {
    mciSendStringW(L"stop HeavyGunnerBgm", nullptr, 0, nullptr);
    mciSendStringW(L"close HeavyGunnerBgm", nullptr, 0, nullptr);
    g_bgmTrack = BgmTrack::None;
    g_bgmLoopCount = 0;
}

BgmTrack selectedMissionBgm() {
    switch (g_selectedStageTheme) {
    case 1U: return BgmTrack::Mission2;
    case 2U: return BgmTrack::Mission3Head;
    case 3U: return BgmTrack::Mission4;
    case 4U: return BgmTrack::Mission5;
    default: return BgmTrack::Mission1Head;
    }
}

void playBgm(BgmTrack track) {
    if (!g_settingSound) {
        stopBgm();
        return;
    }
    if (track == g_bgmTrack || track == BgmTrack::None) {
        return;
    }
    stopBgm();
    const wchar_t* file = L"bgm-title.wav";
    if (track == BgmTrack::Mission1Head) {
        file = L"bgm-mission1-head.wav";
    } else if (track == BgmTrack::Mission1Body) {
        file = L"bgm-mission1-body.wav";
    } else if (track == BgmTrack::Mission2) {
        file = L"bgm-mission2.wav";
    } else if (track == BgmTrack::Mission3Head) {
        file = L"bgm-mission3-head.wav";
    } else if (track == BgmTrack::Mission3Body) {
        file = L"bgm-mission3-body.wav";
    } else if (track == BgmTrack::Mission4) {
        file = L"bgm-mission4.wav";
    } else if (track == BgmTrack::Mission5) {
        file = L"bgm-mission5.wav";
    }
    const auto path = executableDirectory() / L"assets" / file;
    const std::wstring openCommand = L"open \"" + path.wstring() +
        L"\" type waveaudio alias HeavyGunnerBgm";
    const MCIERROR openResult = mciSendStringW(openCommand.c_str(), nullptr, 0, nullptr);
    MCIERROR playResult = openResult;
    if (openResult == 0) {
        playResult = mciSendStringW(L"play HeavyGunnerBgm", nullptr, 0, nullptr);
    }
    std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
    diagnostics << "bgmFile=" << std::filesystem::path(file).string()
                << ",openResult=" << openResult
                << ",playResult=" << playResult << '\n';
    if (openResult == 0 && playResult == 0) {
        g_bgmTrack = track;
    } else {
        stopBgm();
    }
}

void ensureBgmLoop() {
    if (g_bgmTrack == BgmTrack::None) {
        return;
    }
    wchar_t mode[32]{};
    if (mciSendStringW(L"status HeavyGunnerBgm mode", mode,
                       static_cast<UINT>(std::size(mode)), nullptr) != 0 ||
        std::wstring_view(mode) != L"stopped") {
        return;
    }
    if (g_bgmTrack == BgmTrack::Mission1Head) {
        playBgm(BgmTrack::Mission1Body);
        std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
        diagnostics << "bgmTransition=mission1-head-to-body\n";
        return;
    }
    if (g_bgmTrack == BgmTrack::Mission3Head) {
        playBgm(BgmTrack::Mission3Body);
        std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
        diagnostics << "bgmTransition=mission3-head-to-body\n";
        return;
    }
    mciSendStringW(L"seek HeavyGunnerBgm to start", nullptr, 0, nullptr);
    if (mciSendStringW(L"play HeavyGunnerBgm", nullptr, 0, nullptr) == 0) {
        ++g_bgmLoopCount;
        std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
        diagnostics << "bgmLoop=" << g_bgmLoopCount << '\n';
    }
}

bool decodeImage(const std::filesystem::path& path, std::vector<std::uint8_t>& pixels,
                 UINT& width, UINT& height) {
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = g_wicFactory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || FAILED(frame->GetSize(&width, &height))) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = g_wicFactory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) {
        return false;
    }

    const UINT stride = width * 4U;
    pixels.resize(static_cast<std::size_t>(stride) * height);
    return SUCCEEDED(converter->CopyPixels(nullptr, stride,
        static_cast<UINT>(pixels.size()), pixels.data()));
}

bool uploadTexture(const std::filesystem::path& path, GLuint& texture,
                   int& textureWidth, int& textureHeight) {
    std::vector<std::uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    if (!decodeImage(path, pixels, width, height)) {
        return false;
    }
    if (texture == 0) {
        glGenTextures(1, &texture);
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
        static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    textureWidth = static_cast<int>(width);
    textureHeight = static_cast<int>(height);
    return glGetError() == GL_NO_ERROR;
}

bool loadCurrentTexture(HWND window) {
    const auto path = executableDirectory() / L"assets" / kTextures[g_textureIndex].file;
    if (!uploadTexture(path, g_texture, g_textureWidth, g_textureHeight)) {
        g_status = L"WIC decode or OpenGL texture upload failed";
        updateWindowTitle(window);
        return false;
    }

    g_status = std::to_wstring(g_textureWidth) + L"x" + std::to_wstring(g_textureHeight) +
        L"  [Arrows: texture, Space: WAV, mouse: logical input]";
    updateWindowTitle(window);
    return true;
}

bool loadTitleScene(HWND window) {
    const auto assetDirectory = executableDirectory() / L"assets";
    const float limit = std::numeric_limits<float>::max();
    g_sceneMinimum = pact::Vec3{limit, limit, limit};
    g_sceneMaximum = pact::Vec3{-limit, -limit, -limit};
    g_sceneVertices = 0;
    g_sceneTriangles = 0;
    std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
    for (auto& part : g_titleScene) {
        const auto meshPath = assetDirectory / part.meshFile;
        std::ifstream stream(meshPath, std::ios::binary | std::ios::ate);
        const auto length = stream.tellg();
        if (length <= 0) {
            g_status = L"Title scene PMOD was not found";
            updateWindowTitle(window);
            return false;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::string error;
        if (!pact::decodePmodV23(bytes, part.mesh, error)) {
            g_status = L"Title scene PMOD decode failed";
            updateWindowTitle(window);
            return false;
        }
        int width = 0;
        int height = 0;
        if (!uploadTexture(assetDirectory / part.textureFile, part.texture, width, height)) {
            g_status = L"Title scene texture upload failed";
            updateWindowTitle(window);
            return false;
        }
        part.uvScaleU = 256.0F / static_cast<float>(std::max(width, 1));
        part.uvScaleV = 256.0F / static_cast<float>(std::max(height, 1));
        glBindTexture(GL_TEXTURE_2D, part.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        const auto& minimum = part.mesh.bounds[0];
        const auto& maximum = part.mesh.bounds[1];
        g_sceneMinimum.x = std::min(g_sceneMinimum.x, minimum.x);
        g_sceneMinimum.y = std::min(g_sceneMinimum.y, minimum.y);
        g_sceneMinimum.z = std::min(g_sceneMinimum.z, minimum.z);
        g_sceneMaximum.x = std::max(g_sceneMaximum.x, maximum.x);
        g_sceneMaximum.y = std::max(g_sceneMaximum.y, maximum.y);
        g_sceneMaximum.z = std::max(g_sceneMaximum.z, maximum.z);
        g_sceneVertices += part.mesh.positions.size();
        g_sceneTriangles += part.mesh.indices.size() / 3U;
        diagnostics << "scenePart=" << std::filesystem::path(part.meshFile).string()
                    << ",vertices=" << part.mesh.positions.size()
                    << ",triangles=" << (part.mesh.indices.size() / 3U)
                    << ",consumedBytes=" << part.mesh.consumedBytes
                    << ",texture=" << width << 'x' << height << '\n';
    }
    diagnostics << "sceneVertices=" << g_sceneVertices << '\n'
                << "sceneTriangles=" << g_sceneTriangles << '\n';
    return true;
}

bool loadMissionTables(HWND window, std::ofstream& diagnostics) {
    const auto assetDirectory = executableDirectory() / L"assets";
    const auto decodeTable = [&](const wchar_t* file, pact::A3dTable& table) {
        std::ifstream stream(assetDirectory / file, std::ios::binary | std::ios::ate);
        const auto length = stream.tellg();
        if (length <= 0) {
            return false;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        std::string error;
        return pact::decodeA3d(bytes, table, error);
    };
    if (!decodeTable(L"hg-stage.a3d", g_stageTable) ||
        !decodeTable(L"hg-enemy.a3d", g_enemyTable) ||
        !decodeTable(L"hg-enemy-pattern.a3d", g_enemyPatternTable) ||
        g_stageTable.rowCount < 25U ||
        g_enemyTable.rowCount < 21U ||
        g_enemyPatternTable.rowCount < 55U) {
        g_status = L"Recovered A3D mission tables failed validation";
        updateWindowTitle(window);
        return false;
    }

    const auto enemyModelForType = [](int enemyType) -> std::size_t {
        if (enemyType <= 3) return 0U;
        if (enemyType <= 6) return 1U;
        if (enemyType <= 9) return 2U;
        if (enemyType <= 11) return 3U;
        if (enemyType == 12 || enemyType == 15) return 4U;
        if (enemyType <= 15) return 5U;
        return 6U;
    };
    const auto widen = [](const std::string& text) {
        return std::wstring(text.begin(), text.end());
    };

    g_campaignMissions.clear();
    g_campaignMissions.reserve(25U);
    for (std::size_t row = 0; row < 25U; ++row) {
        const int objectiveCode = g_stageTable.signedValue(row, 6U, 2);
        MissionObjective objective = MissionObjective::DestroyTargets;
        if (objectiveCode == 1) {
            objective = MissionObjective::Survive;
        } else if (objectiveCode == 3) {
            objective = MissionObjective::Boss;
        }
        int enemyType = g_stageTable.signedValue(row, 8U, 1);
        if (objective == MissionObjective::Boss) {
            for (std::size_t field = 8U; field <= 13U; ++field) {
                const int candidate = g_stageTable.signedValue(row, field, 0);
                if (candidate >= 16) {
                    enemyType = candidate;
                    break;
                }
            }
        }
        enemyType = std::clamp(enemyType, 0, 20);
        const int rawHitPoints = g_enemyTable.signedValue(
            static_cast<std::size_t>(enemyType), 3U, 100);
        int hitPoints = std::max(3, rawHitPoints / 500);
        if (objective == MissionObjective::Boss) {
            hitPoints = std::clamp(hitPoints, 40, 100);
        } else {
            hitPoints = std::clamp(hitPoints, 3, 12);
        }
        const int rawPattern = g_stageTable.signedValue(row, 14U, 0);
        const std::size_t patternId = static_cast<std::size_t>(
            std::clamp(rawPattern, 0,
                static_cast<int>(g_enemyPatternTable.rowCount) - 1));
        const int patternGroup = g_enemyPatternTable.signedValue(patternId, 8U, 10);
        const int movementType = g_enemyPatternTable.signedValue(patternId, 1U, 1);
        const int attackMode = g_enemyPatternTable.signedValue(patternId, 9U, 1);
        const int movementDuration =
            g_enemyPatternTable.signedValue(patternId, 16U, 0);
        const int attackWindow =
            g_enemyPatternTable.signedValue(patternId, 17U, 0);
        const std::size_t activeEnemies = objective == MissionObjective::Boss
            ? 1U : static_cast<std::size_t>(std::clamp(patternGroup, 1, 18));
        const std::string titleText = g_stageTable.stringValue(row, 5U);
        const int targetCount = objective == MissionObjective::Boss
            ? 1 : std::max(0, g_stageTable.signedValue(row, 2U, 0));
        MissionSpec mission{
            titleText.empty() ? L"UNTITLED" : widen(titleText),
            objective,
            std::max(1, g_stageTable.signedValue(row, 1U, 300)),
            targetCount,
            enemyModelForType(enemyType),
            hitPoints,
            enemyType,
            patternId,
            activeEnemies,
        };
        diagnostics << "a3dMission=" << row
                    << ",title=" << titleText
                    << ",objective=" << objectiveCode
                    << ",seconds=" << mission.durationSeconds
                    << ",targets=" << mission.targetCount
                    << ",enemyType=" << mission.enemyTypeId
                    << ",enemyModel=" << mission.enemyModel
                    << ",enemyHp=" << mission.enemyHitPoints
                    << ",pattern=" << mission.patternId
                    << ",movement=" << movementType
                    << ",attack=" << attackMode
                    << ",moveMs=" << movementDuration
                    << ",attackWindowMs=" << attackWindow
                    << ",active=" << mission.maxActiveEnemies << '\n';
        g_campaignMissions.push_back(std::move(mission));
    }
    diagnostics << "a3dCampaignMissions=" << g_campaignMissions.size() << '\n';
    return true;
}

bool loadMissionScene(HWND window) {
    const auto assetDirectory = executableDirectory() / L"assets";
    std::size_t vertices = 0;
    std::size_t triangles = 0;
    std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
    if (!loadMissionTables(window, diagnostics)) {
        return false;
    }
    const auto loadEnvironment = [&](auto& scene, const char* stageLabel) {
        for (auto& part : scene) {
            const auto meshPath = assetDirectory / part.meshFile;
            std::ifstream stream(meshPath, std::ios::binary | std::ios::ate);
            const auto length = stream.tellg();
            if (length <= 0) {
                return false;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
            std::string error;
            if (!pact::decodePmodV23(bytes, part.mesh, error)) {
                return false;
            }
            int width = 0;
            int height = 0;
            if (!uploadTexture(assetDirectory / part.textureFile, part.texture, width, height)) {
                return false;
            }
            part.uvScaleU = 256.0F / static_cast<float>(std::max(width, 1));
            part.uvScaleV = 256.0F / static_cast<float>(std::max(height, 1));
            glBindTexture(GL_TEXTURE_2D, part.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            vertices += part.mesh.positions.size();
            triangles += part.mesh.indices.size() / 3U;
            diagnostics << "missionPart=" << stageLabel << ':'
                        << std::filesystem::path(part.meshFile).string()
                        << ",vertices=" << part.mesh.positions.size()
                        << ",triangles=" << (part.mesh.indices.size() / 3U)
                        << ",texture=" << width << 'x' << height << '\n';
        }
        return true;
    };
    if (!loadEnvironment(g_missionScene, "1-1") ||
        !loadEnvironment(g_missionScene2, "2-1") ||
        !loadEnvironment(g_missionScene3, "3-1") ||
        !loadEnvironment(g_missionScene4, "4-1") ||
        !loadEnvironment(g_missionScene5, "5-1")) {
        g_status = L"Recovered mission environment PMOD or texture load failed";
        updateWindowTitle(window);
        return false;
    }
    const auto loadActor = [&](ScenePart& part, const char* label) {
        std::ifstream stream(assetDirectory / part.meshFile, std::ios::binary | std::ios::ate);
        const auto length = stream.tellg();
        if (length <= 0) {
            return false;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        std::string error;
        if (!pact::decodePmodV23(bytes, part.mesh, error)) {
            return false;
        }
        int width = 0;
        int height = 0;
        if (!uploadTexture(assetDirectory / part.textureFile, part.texture, width, height)) {
            return false;
        }
        part.uvScaleU = 256.0F / static_cast<float>(std::max(width, 1));
        part.uvScaleV = 256.0F / static_cast<float>(std::max(height, 1));
        glBindTexture(GL_TEXTURE_2D, part.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        diagnostics << label << "Vertices=" << part.mesh.positions.size() << '\n'
                    << label << "Triangles=" << (part.mesh.indices.size() / 3U) << '\n';
        return true;
    };
    constexpr std::array<const char*, 7> enemyLabels{{
        "enemy-zako01", "enemy-zako02", "enemy-zako03", "enemy-middle02",
        "enemy-bmissile", "enemy-middle01", "enemy-boss01"}};
    for (std::size_t index = 0; index < g_enemySceneParts.size(); ++index) {
        if (!loadActor(g_enemySceneParts[index], enemyLabels[index])) {
            g_status = L"Mission actor PMOD or texture load failed";
            updateWindowTitle(window);
            return false;
        }
    }
    const auto loadEnemyBoneAnimation =
        [&](std::size_t enemyIndex, const wchar_t* file) {
            std::ifstream stream(assetDirectory / file,
                                 std::ios::binary | std::ios::ate);
            const auto length = stream.tellg();
            if (length <= 0) return false;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
            if (!stream.good()) return false;

            auto& animation = g_enemyVertexAnimations[enemyIndex];
            std::string error;
            if (!pact::decodePlivV23Header(bytes, animation.action, error) ||
                (animation.action.combinedFeatureFlags &
                 pact::kPlivBoneAnimation) == 0U ||
                !pact::decodePlivV23Bone(bytes, animation.action,
                                        animation.action.payloadOffset,
                                        animation.boneSection, error) ||
                !buildEnemyBoneFrames(g_enemySceneParts[enemyIndex], animation,
                                      error)) {
                diagnostics << enemyLabels[enemyIndex]
                            << "BoneAnimationError=" << error << '\n';
                return false;
            }
            const auto& motion = animation.boneSection.motions.front();
            const auto& descriptor = animation.action.motions[motion.motionIndex];
            std::size_t ownedVertices = 0U;
            for (const auto& bone : animation.boneSection.header.bones) {
                ownedVertices += bone.rawField;
            }
            std::vector<bool> changedVertices(ownedVertices, false);
            float maximumDisplacementSquared = 0.0F;
            for (std::size_t frame = 1U; frame < animation.boneFrames.size(); ++frame) {
                for (std::size_t vertex = 0U; vertex < ownedVertices; ++vertex) {
                    const auto& origin = animation.boneFrames.front()[vertex];
                    const auto& sample = animation.boneFrames[frame][vertex];
                    const float dx = sample.x - origin.x;
                    const float dy = sample.y - origin.y;
                    const float dz = sample.z - origin.z;
                    const float distanceSquared = dx * dx + dy * dy + dz * dz;
                    maximumDisplacementSquared =
                        std::max(maximumDisplacementSquared, distanceSquared);
                    if (distanceSquared > 0.000001F) changedVertices[vertex] = true;
                }
            }
            const std::size_t changedVertexCount = static_cast<std::size_t>(
                std::count(changedVertices.begin(), changedVertices.end(), true));
            diagnostics << enemyLabels[enemyIndex]
                        << "BoneAnimationBones="
                        << animation.boneSection.header.bones.size() << '\n'
                        << enemyLabels[enemyIndex]
                        << "BoneAnimationFrames=" << descriptor.frameCount << '\n'
                        << enemyLabels[enemyIndex]
                        << "BoneAnimationFps="
                        << static_cast<unsigned>(descriptor.framesPerSecond) << '\n'
                        << enemyLabels[enemyIndex]
                        << "BoneAnimationVertices=" << ownedVertices << '\n'
                        << enemyLabels[enemyIndex]
                        << "BoneAnimationChangedVertices="
                        << changedVertexCount << '\n'
                        << enemyLabels[enemyIndex]
                        << "BoneAnimationMaximumDisplacement="
                        << std::sqrt(maximumDisplacementSquared) << '\n'
                        << enemyLabels[enemyIndex]
                        << "BoneAnimationConsumed="
                        << animation.boneSection.consumedBytes << '\n';
            return true;
        };
    if (!loadEnemyBoneAnimation(0U, L"enemy-zako01.pliv") ||
        !loadEnemyBoneAnimation(1U, L"enemy-zako02.pliv") ||
        !loadEnemyBoneAnimation(2U, L"enemy-zako03.pliv")) {
        g_status = L"Recovered enemy PLIV bone animation decode failed";
        updateWindowTitle(window);
        return false;
    }
    const auto loadEnemyVertexAnimation =
        [&](std::size_t enemyIndex, const wchar_t* file) {
            std::ifstream stream(assetDirectory / file,
                                 std::ios::binary | std::ios::ate);
            const auto length = stream.tellg();
            if (length <= 0) {
                return false;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
            if (!stream.good()) {
                return false;
            }
            auto& animation = g_enemyVertexAnimations[enemyIndex];
            std::string error;
            if (!pact::decodePlivV23Header(bytes, animation.action, error)) {
                return false;
            }
            constexpr std::array<std::uint8_t, 4> vertexMarker{{
                '*', '*', '*', '*'}};
            const auto marker = std::search(bytes.begin(), bytes.end(),
                                            vertexMarker.begin(),
                                            vertexMarker.end());
            if (marker == bytes.end()) {
                return false;
            }
            const std::size_t vertexOffset = static_cast<std::size_t>(
                std::distance(bytes.begin(), marker));
            if (!pact::decodePlivV23Vertex(bytes, animation.action, vertexOffset,
                                          animation.section, error)) {
                return false;
            }
            animation.loaded = true;
            const auto& motion = animation.section.motions.front();
            const auto& descriptor =
                animation.action.motions[motion.motionIndex];
            diagnostics << enemyLabels[enemyIndex]
                        << "VertexAnimationFrames=" << motion.frames.size() << '\n'
                        << enemyLabels[enemyIndex]
                        << "VertexAnimationFps="
                        << static_cast<unsigned>(descriptor.framesPerSecond) << '\n'
                        << enemyLabels[enemyIndex]
                        << "VertexAnimationVertices=" << motion.texcoordCount << '\n'
                        << enemyLabels[enemyIndex]
                        << "VertexAnimationTriangles="
                        << (motion.remapCount / 3U) << '\n'
                        << enemyLabels[enemyIndex]
                        << "VertexAnimationConsumed="
                        << animation.section.consumedBytes << '\n';
            return true;
        };
    if (!loadEnemyVertexAnimation(0U, L"enemy-zako01.pliv") ||
        !loadEnemyVertexAnimation(4U, L"enemy-bmissile.pliv")) {
        g_status = L"Recovered enemy PLIV vertex animation decode failed";
        updateWindowTitle(window);
        return false;
    }
    constexpr std::array<const char*, 8> weaponLabels{{
        "lmg", "shower", "chaser", "missile",
        "emp", "hmg", "shield", "thunder"}};
    for (std::size_t index = 0; index < g_weaponSceneParts.size(); ++index) {
        if (!loadActor(g_weaponSceneParts[index], weaponLabels[index])) {
            g_status = L"Recovered player weapon PMOD or texture load failed";
            updateWindowTitle(window);
            return false;
        }
    }
    const auto loadEffectTexture = [&](const wchar_t* file, GLuint& texture) {
        int width = 0;
        int height = 0;
        if (!uploadTexture(assetDirectory / file, texture, width, height)) {
            return false;
        }
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        diagnostics << "effectTexture=" << std::filesystem::path(file).string()
                    << ',' << width << 'x' << height << '\n';
        return true;
    };
    constexpr std::array<const wchar_t*, 4> hitFiles{{
        L"effect-hit-01.png", L"effect-hit-02.png",
        L"effect-hit-03.png", L"effect-hit-04.png"}};
    for (std::size_t index = 0; index < hitFiles.size(); ++index) {
        if (!loadEffectTexture(hitFiles[index], g_hitEffectTextures[index])) return false;
    }
    if (!loadEffectTexture(L"effect-enemy-die.png", g_enemyDieTexture) ||
        !loadEffectTexture(L"effect-player-hit.png", g_playerHitTexture) ||
        !loadEffectTexture(L"effect-player-shield-hit.png", g_playerShieldHitTexture) ||
        !loadEffectTexture(L"effect-missile-explode.png", g_missileExplosionTexture) ||
        !loadEffectTexture(L"effect-emp-field01.png", g_empFieldTextures[0]) ||
        !loadEffectTexture(L"effect-emp-field02.png", g_empFieldTextures[1]) ||
        !loadEffectTexture(L"effect-shield-field.png", g_shieldFieldTexture) ||
        !loadEffectTexture(L"effect-thunder-fire.png", g_thunderFireTexture) ||
        !loadEffectTexture(L"effect-boss-laser.png", g_bossLaserTexture)) {
        g_status = L"Recovered combat effect texture load failed";
        updateWindowTitle(window);
        return false;
    }
    const auto loadEffectDescriptor = [&](const wchar_t* file,
                                          pact::EffectDescriptor& descriptor) {
        std::ifstream stream(assetDirectory / file, std::ios::binary | std::ios::ate);
        const auto length = stream.tellg();
        if (length <= 0) return false;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        std::string error;
        if (!stream || !pact::decodePeftV23(bytes, descriptor, error) ||
            descriptor.motions.empty()) {
            return false;
        }
        const auto& motion = descriptor.motions.front();
        const ULONGLONG duration =
            (static_cast<ULONGLONG>(motion.frameCount) * 1000ULL +
             motion.framesPerSecond - 1ULL) / motion.framesPerSecond;
        diagnostics << "peftEffect=" << std::filesystem::path(file).string()
                    << ",fps=" << static_cast<unsigned>(motion.framesPerSecond)
                    << ",frames=" << motion.frameCount
                    << ",durationMs=" << duration
                    << ",consumed=" << descriptor.vertexPrefix.consumedBytes << '\n';
        return true;
    };
    struct EffectDescriptorAsset {
        CombatEffect effect;
        const wchar_t* file;
    };
    constexpr std::array<EffectDescriptorAsset, 5> effectDescriptorAssets{{
        {CombatEffect::Hit, L"effect-hit.peft"},
        {CombatEffect::EnemyDie, L"effect-enemy-die.peft"},
        {CombatEffect::MissileExplosion, L"effect-missile-explode.peft"},
        {CombatEffect::Shield, L"effect-shield-field.peft"},
        {CombatEffect::Thunder, L"effect-thunder-fire.peft"},
    }};
    for (const auto& asset : effectDescriptorAssets) {
        const auto index = static_cast<std::size_t>(asset.effect);
        if (!loadEffectDescriptor(asset.file, g_combatEffectDescriptors[index])) {
            g_status = L"Recovered PEFT effect metadata load failed";
            updateWindowTitle(window);
            return false;
        }
        g_combatEffectDescriptorLoaded[index] = true;
    }
    if (!loadEffectDescriptor(L"effect-boss-laser.peft",
                              g_bossLaserEffectDescriptor)) {
        g_status = L"Recovered boss-laser PEFT metadata load failed";
        updateWindowTitle(window);
        return false;
    }
    g_bossLaserEffectDescriptorLoaded = true;
    diagnostics << "missionVertices=" << vertices << '\n'
                << "missionTriangles=" << triangles << '\n';
    g_missionSceneLoaded = true;
    return true;
}

bool loadTitleCamera(HWND window) {
    const auto path = executableDirectory() / L"assets" / L"title-camera.pcam";
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto length = stream.tellg();
    if (length <= 0) {
        g_status = L"Title PCAM was not found";
        updateWindowTitle(window);
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::string error;
    if (!pact::decodePcamV23(bytes, g_titleCamera, error)) {
        g_status = L"Title PCAM decode failed";
        updateWindowTitle(window);
        return false;
    }
    g_cameraStartTime = GetTickCount64();
    g_cameraLoaded = true;
    const auto& position = g_titleCamera.positions.front();
    const auto& target = g_titleCamera.targets.front();
    std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
    diagnostics << "cameraFrames=" << g_titleCamera.frameCount << '\n'
                << "cameraFps=" << static_cast<unsigned>(g_titleCamera.framesPerSecond) << '\n'
                << "cameraFov=" << g_titleCamera.fieldOfViewDegrees << '\n'
                << "cameraPosition0=" << position.x << ',' << position.y << ',' << position.z << '\n'
                << "cameraTarget0=" << target.x << ',' << target.y << ',' << target.z << '\n'
                << "cameraConsumedBytes=" << g_titleCamera.consumedBytes << '\n';
    return true;
}

bool loadGunFireAnimation(HWND window) {
    const auto assetDirectory = executableDirectory() / L"assets";
    const auto readBinary = [](const std::filesystem::path& path,
                               std::vector<std::uint8_t>& bytes) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        const auto length = stream.tellg();
        if (length <= 0) {
            return false;
        }
        bytes.resize(static_cast<std::size_t>(length));
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        return stream.good();
    };
    std::vector<std::uint8_t> meshBytes;
    std::vector<std::uint8_t> actionBytes;
    if (!readBinary(assetDirectory / L"gun-fire.pmod", meshBytes) ||
        !readBinary(assetDirectory / L"gun-fire.pliv", actionBytes)) {
        g_status = L"gun_fire PMOD/PLIV was not found";
        updateWindowTitle(window);
        return false;
    }
    std::string error;
    if (!pact::decodePmodV23(meshBytes, g_gunFireMesh, error) ||
        !pact::decodePlivV23Header(actionBytes, g_gunFireAction, error) ||
        !pact::decodePlivV23UnflowedTexture(actionBytes, g_gunFireAction,
            g_gunFireAction.payloadOffset, g_gunFireAnimation, error) ||
        g_gunFireAnimation.motions.empty() ||
        g_gunFireAnimation.motions.front().tracks.empty()) {
        g_status = L"gun_fire PMOD/PLIV decode failed";
        updateWindowTitle(window);
        return false;
    }
    int width = 0;
    int height = 0;
    if (!uploadTexture(assetDirectory / L"gun-fire.png", g_gunFireTexture, width, height)) {
        g_status = L"gun_fire texture upload failed";
        updateWindowTitle(window);
        return false;
    }
    g_gunFireStartTime = GetTickCount64();
    g_gunFireLoaded = true;
    const auto& animationMotion = g_gunFireAnimation.motions.front();
    const auto& descriptor = g_gunFireAction.motions[animationMotion.motionIndex];
    const auto& track = animationMotion.tracks.front();
    std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
    diagnostics << "gunFireVertices=" << g_gunFireMesh.positions.size() << '\n'
                << "gunFireTriangles=" << (g_gunFireMesh.indices.size() / 3U) << '\n'
                << "gunFireFrames=" << descriptor.frameCount << '\n'
                << "gunFireFps=" << static_cast<unsigned>(descriptor.framesPerSecond) << '\n'
                << "gunFireUvKeys=" << track.keys.size() << '\n'
                << "gunFireTexture=" << width << 'x' << height << '\n';
    return true;
}

bool loadGameUi(HWND window) {
    const auto assetDirectory = executableDirectory() / L"assets";
    int width = 0;
    int height = 0;
    const bool loaded =
        uploadTexture(assetDirectory / L"title-ui.png", g_titleUiTexture, width, height) &&
        uploadTexture(assetDirectory / L"title-overlay-en.png", g_menuUiTexture, width, height) &&
        uploadTexture(assetDirectory / L"cutscene-01-en.png", g_storyTextures[0], width, height) &&
        uploadTexture(assetDirectory / L"cutscene-02-en.png", g_storyTextures[1], width, height) &&
        uploadTexture(assetDirectory / L"cutscene-03-en.png", g_storyTextures[2], width, height) &&
        uploadTexture(assetDirectory / L"cutscene-04-en.png", g_storyTextures[3], width, height) &&
        uploadTexture(assetDirectory / L"cutscene-05-en.png", g_storyTextures[4], width, height) &&
        uploadTexture(assetDirectory / L"target-01.png", g_targetTextures[0], width, height) &&
        uploadTexture(assetDirectory / L"target-02.png", g_targetTextures[1], width, height) &&
        uploadTexture(assetDirectory / L"target-03.png", g_targetTextures[2], width, height) &&
        uploadTexture(assetDirectory / L"target-04.png", g_targetTextures[3], width, height) &&
        uploadTexture(assetDirectory / L"target-06.png", g_targetTextures[4], width, height) &&
        uploadTexture(assetDirectory / L"target-08.png", g_targetTextures[5], width, height) &&
        uploadTexture(assetDirectory / L"armory-reference.png", g_armoryTexture, width, height) &&
        uploadTexture(assetDirectory / L"armory-confirm-reference.png",
                      g_armoryConfirmTexture, width, height) &&
        uploadTexture(assetDirectory / L"armory-ui-01.png",
                      g_armoryUi1Texture, width, height) &&
        uploadTexture(assetDirectory / L"armory-ui-02.png",
                      g_armoryUi2Texture, width, height) &&
        uploadTexture(assetDirectory / L"armory-ui-03.png",
                      g_armoryUi3Texture, width, height) &&
        uploadTexture(assetDirectory / L"popup-en.png",
                      g_popupEnTexture, width, height) &&
        uploadTexture(assetDirectory / L"gameover-en.png",
                      g_gameoverTexture, width, height) &&
        uploadTexture(assetDirectory / L"gameplay-reference.png",
                      g_gameplayReferenceTexture, width, height) &&
        uploadTexture(assetDirectory / L"paused-reference.png",
                      g_pausedReferenceTexture, width, height) &&
        uploadTexture(assetDirectory / L"cockpit-results.png", g_resultsTexture, width, height) &&
        uploadTexture(assetDirectory / L"cockpit-en.png", g_cockpitTexture, width, height) &&
        uploadTexture(assetDirectory / L"pause-popup-kor.png", g_pauseTexture, width, height);
    if (!loaded) {
        g_status = L"Recovered game UI texture load failed";
        updateWindowTitle(window);
    }
    return loaded;
}

pact::Vec3 subtract(const pact::Vec3& left, const pact::Vec3& right) {
    return pact::Vec3{left.x - right.x, left.y - right.y, left.z - right.z};
}

pact::Vec3 normalize(const pact::Vec3& value) {
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length <= 0.000001f) {
        return pact::Vec3{};
    }
    return pact::Vec3{value.x / length, value.y / length, value.z / length};
}

pact::Vec3 cross(const pact::Vec3& left, const pact::Vec3& right) {
    return pact::Vec3{left.y * right.z - left.z * right.y,
                      left.z * right.x - left.x * right.z,
                      left.x * right.y - left.y * right.x};
}

void applyPerspective(float fieldOfViewDegrees, float aspect, float nearPlane, float farPlane) {
    constexpr double pi = 3.14159265358979323846;
    const double top = static_cast<double>(nearPlane) *
        std::tan(static_cast<double>(fieldOfViewDegrees) * pi / 360.0);
    const double right = top * static_cast<double>(aspect);
    glFrustum(-right, right, -top, top, nearPlane, farPlane);
}

void applyLookAt(const pact::Vec3& position, const pact::Vec3& target, float rollDegrees) {
    const pact::Vec3 forward = normalize(subtract(target, position));
    pact::Vec3 side = normalize(cross(forward, pact::Vec3{0.0f, 1.0f, 0.0f}));
    if (std::abs(side.x) + std::abs(side.y) + std::abs(side.z) < 0.000001f) {
        side = pact::Vec3{1.0f, 0.0f, 0.0f};
    }
    const pact::Vec3 up = cross(side, forward);
    const GLfloat matrix[16]{
        side.x, up.x, -forward.x, 0.0f,
        side.y, up.y, -forward.y, 0.0f,
        side.z, up.z, -forward.z, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    if (rollDegrees != 0.0f) {
        glRotatef(-rollDegrees, 0.0f, 0.0f, 1.0f);
    }
    glMultMatrixf(matrix);
    glTranslatef(-position.x, -position.y, -position.z);
}

RECT logicalViewport() {
    constexpr float targetAspect = 5.0F / 3.0F;
    const float windowAspect = static_cast<float>(std::max(g_clientWidth, 1)) /
        static_cast<float>(std::max(g_clientHeight, 1));
    int width = g_clientWidth;
    int height = g_clientHeight;
    int left = 0;
    int top = 0;
    if (windowAspect > targetAspect) {
        width = static_cast<int>(std::lround(static_cast<float>(height) * targetAspect));
        left = (g_clientWidth - width) / 2;
    } else {
        height = static_cast<int>(std::lround(static_cast<float>(width) / targetAspect));
        top = (g_clientHeight - height) / 2;
    }
    return RECT{left, top, left + width, top + height};
}

bool captureViewportBmp(const RECT& viewport) {
    const int width = viewport.right - viewport.left;
    const int height = viewport.bottom - viewport.top;
    if (width <= 0 || height <= 0) {
        return false;
    }
    const std::size_t sourceStride = static_cast<std::size_t>(width) * 3U;
    const std::size_t bitmapStride = (sourceStride + 3U) & ~3U;
    std::vector<std::uint8_t> rgb(sourceStride * static_cast<std::size_t>(height));
    std::vector<std::uint8_t> bitmap(bitmapStride * static_cast<std::size_t>(height), 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(viewport.left, g_clientHeight - viewport.bottom, width, height,
                 GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    if (glGetError() != GL_NO_ERROR) {
        return false;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t source = static_cast<std::size_t>(y) * sourceStride +
                static_cast<std::size_t>(x) * 3U;
            const std::size_t destination = static_cast<std::size_t>(y) * bitmapStride +
                static_cast<std::size_t>(x) * 3U;
            bitmap[destination] = rgb[source + 2U];
            bitmap[destination + 1U] = rgb[source + 1U];
            bitmap[destination + 2U] = rgb[source];
        }
    }
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER infoHeader{};
    fileHeader.bfType = 0x4D42U;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(bitmap.size());
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = width;
    infoHeader.biHeight = height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 24;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = static_cast<DWORD>(bitmap.size());
    const char* captureName = "opengl-mesh-capture.bmp";
    if (g_fireMode) {
        captureName = "opengl-fire-capture.bmp";
    } else if (g_gameFlow) {
        switch (g_runtimeState) {
        case RuntimeState::Title: captureName = "opengl-game-title-capture.bmp"; break;
        case RuntimeState::MainMenu: captureName = "opengl-game-menu-capture.bmp"; break;
        case RuntimeState::Challenge:
            captureName = "opengl-game-challenge-capture.bmp"; break;
        case RuntimeState::Extra: captureName = "opengl-game-extra-capture.bmp"; break;
        case RuntimeState::Settings:
            captureName = "opengl-game-settings-capture.bmp"; break;
        case RuntimeState::StageSelect:
            captureName = "opengl-game-stage-capture.bmp"; break;
        case RuntimeState::Story: captureName = "opengl-game-story-capture.bmp"; break;
        case RuntimeState::Armory: captureName = "opengl-game-armory-capture.bmp"; break;
        case RuntimeState::ArmoryConfirm:
            captureName = "opengl-game-armory-confirm-capture.bmp"; break;
        case RuntimeState::Playing: captureName = "opengl-game-playing-capture.bmp"; break;
        case RuntimeState::Paused: captureName = "opengl-game-paused-capture.bmp"; break;
        }
    }
    if (g_gameFlow && g_runtimeState == RuntimeState::Playing && g_missionFailed) {
        captureName = "opengl-game-failed-capture.bmp";
    }
    std::ofstream output(executableDirectory() / captureName, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    output.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
    output.write(reinterpret_cast<const char*>(bitmap.data()),
                 static_cast<std::streamsize>(bitmap.size()));
    return output.good();
}

void drawLogicalTexture(GLuint texture, float x, float y, float width, float height,
                        float alpha = 1.0F) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4f(1.0F, 1.0F, 1.0F, alpha);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0F, 0.0F); glVertex2f(x, y);
    glTexCoord2f(1.0F, 0.0F); glVertex2f(x + width, y);
    glTexCoord2f(1.0F, 1.0F); glVertex2f(x + width, y + height);
    glTexCoord2f(0.0F, 1.0F); glVertex2f(x, y + height);
    glEnd();
}

void drawColoredRect(float x, float y, float width, float height,
                     float red, float green, float blue, float alpha = 1.0F) {
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(red, green, blue, alpha);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + width, y);
    glVertex2f(x + width, y + height);
    glVertex2f(x, y + height);
    glEnd();
}

void drawTextureRegionSized(GLuint texture, float textureWidth, float textureHeight,
                            float sourceX, float sourceY,
                            float sourceWidth, float sourceHeight,
                            float x, float y, float width, float height,
                            float alpha = 1.0F) {
    const float u0 = sourceX / textureWidth;
    const float v0 = sourceY / textureHeight;
    const float u1 = (sourceX + sourceWidth) / textureWidth;
    const float v1 = (sourceY + sourceHeight) / textureHeight;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4f(1.0F, 1.0F, 1.0F, alpha);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x, y);
    glTexCoord2f(u1, v0); glVertex2f(x + width, y);
    glTexCoord2f(u1, v1); glVertex2f(x + width, y + height);
    glTexCoord2f(u0, v1); glVertex2f(x, y + height);
    glEnd();
}

void drawTextureRegionTinted(GLuint texture,
                             float sourceX, float sourceY,
                             float sourceWidth, float sourceHeight,
                             float x, float y, float width, float height,
                             float red, float green, float blue,
                             float alpha = 1.0F) {
    constexpr float atlasSize = 512.0F;
    const float u0 = sourceX / atlasSize;
    const float v0 = sourceY / atlasSize;
    const float u1 = (sourceX + sourceWidth) / atlasSize;
    const float v1 = (sourceY + sourceHeight) / atlasSize;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4f(red, green, blue, alpha);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x, y);
    glTexCoord2f(u1, v0); glVertex2f(x + width, y);
    glTexCoord2f(u1, v1); glVertex2f(x + width, y + height);
    glTexCoord2f(u0, v1); glVertex2f(x, y + height);
    glEnd();
}

void drawTextureRegion(GLuint texture, float sourceX, float sourceY,
                       float sourceWidth, float sourceHeight,
                       float x, float y, float width, float height,
                       float alpha = 1.0F) {
    constexpr float atlasSize = 512.0F;
    const float u0 = sourceX / atlasSize;
    const float v0 = sourceY / atlasSize;
    const float u1 = (sourceX + sourceWidth) / atlasSize;
    const float v1 = (sourceY + sourceHeight) / atlasSize;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4f(1.0F, 1.0F, 1.0F, alpha);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x, y);
    glTexCoord2f(u1, v0); glVertex2f(x + width, y);
    glTexCoord2f(u1, v1); glVertex2f(x + width, y + height);
    glTexCoord2f(u0, v1); glVertex2f(x, y + height);
    glEnd();
}

void drawHudText(const std::string& text, float x, float y, float scale = 1.0F) {
    const auto glyphRows = [](unsigned char character) {
        using Rows = std::array<std::uint8_t, 7>;
        switch (character) {
        case 'T': return Rows{{31, 4, 4, 4, 4, 4, 4}};
        case 'A': return Rows{{14, 17, 17, 31, 17, 17, 17}};
        case 'R': return Rows{{30, 17, 17, 30, 20, 18, 17}};
        case 'G': return Rows{{14, 17, 16, 23, 17, 17, 14}};
        case 'E': return Rows{{31, 16, 16, 30, 16, 16, 31}};
        case 'N': return Rows{{17, 25, 21, 19, 17, 17, 17}};
        case 'O': return Rows{{14, 17, 17, 17, 17, 17, 14}};
        case 'M': return Rows{{17, 27, 21, 21, 17, 17, 17}};
        case 'L': return Rows{{16, 16, 16, 16, 16, 16, 31}};
        case 'H': return Rows{{17, 17, 17, 31, 17, 17, 17}};
        case 'D': return Rows{{30, 17, 17, 17, 17, 17, 30}};
        case 'X': return Rows{{17, 17, 10, 4, 10, 17, 17}};
        case 'B': return Rows{{30, 17, 17, 30, 17, 17, 30}};
        case 'C': return Rows{{14, 17, 16, 16, 16, 17, 14}};
        case 'I': return Rows{{14, 4, 4, 4, 4, 4, 14}};
        case 'U': return Rows{{17, 17, 17, 17, 17, 17, 14}};
        case 'V': return Rows{{17, 17, 17, 17, 17, 10, 4}};
        case 'F': return Rows{{31, 16, 16, 30, 16, 16, 16}};
        case 'P': return Rows{{30, 17, 17, 30, 16, 16, 16}};
        case 'Q': return Rows{{14, 17, 17, 17, 21, 18, 13}};
        case 'W': return Rows{{17, 17, 17, 21, 21, 27, 17}};
        case 'Y': return Rows{{17, 17, 10, 4, 4, 4, 4}};
        case 'K': return Rows{{17, 18, 20, 24, 20, 18, 17}};
        case 'S': return Rows{{15, 16, 16, 14, 1, 1, 30}};
        case '0': return Rows{{14, 17, 19, 21, 25, 17, 14}};
        case '1': return Rows{{4, 12, 4, 4, 4, 4, 14}};
        case '2': return Rows{{14, 17, 1, 2, 4, 8, 31}};
        case '3': return Rows{{30, 1, 1, 14, 1, 1, 30}};
        case '4': return Rows{{2, 6, 10, 18, 31, 2, 2}};
        case '5': return Rows{{31, 16, 16, 30, 1, 1, 30}};
        case '6': return Rows{{14, 16, 16, 30, 17, 17, 14}};
        case '7': return Rows{{31, 1, 2, 4, 8, 8, 8}};
        case '8': return Rows{{14, 17, 17, 14, 17, 17, 14}};
        case '9': return Rows{{14, 17, 17, 15, 1, 1, 14}};
        case ':': return Rows{{0, 4, 4, 0, 4, 4, 0}};
        case '-': return Rows{{0, 0, 0, 31, 0, 0, 0}};
        case '?': return Rows{{14, 17, 1, 2, 4, 0, 4}};
        default: return Rows{{0, 0, 0, 0, 0, 0, 0}};
        }
    };
    float cursor = x;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glColor4f(0.82F, 0.9F, 1.0F, 1.0F);
    for (const unsigned char character : text) {
        const auto rows = glyphRows(character);
        glBegin(GL_QUADS);
        for (std::size_t row = 0; row < rows.size(); ++row) {
            for (unsigned column = 0; column < 5U; ++column) {
                if ((rows[row] & (1U << (4U - column))) == 0U) {
                    continue;
                }
                const float left = cursor + static_cast<float>(column) * scale;
                const float top = y + static_cast<float>(row) * scale;
                glVertex2f(left, top);
                glVertex2f(left + scale, top);
                glVertex2f(left + scale, top + scale);
                glVertex2f(left, top + scale);
            }
        }
        glEnd();
        cursor += 6.0F * scale;
    }
}

void drawCenteredHudText(const std::string& text, float centerX, float y,
                         float scale = 1.0F) {
    const float textWidth = static_cast<float>(text.size()) * 6.0F * scale;
    drawHudText(text, centerX - textWidth * 0.5F, y, scale);
}

void drawMissionCompleteOverlay() {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.0F, 0.0F, 0.0F, 0.52F);
    glBegin(GL_QUADS);
    glVertex2f(0.0F, 0.0F);
    glVertex2f(kLogicalWidth, 0.0F);
    glVertex2f(kLogicalWidth, kLogicalHeight);
    glVertex2f(0.0F, kLogicalHeight);
    glEnd();

    constexpr float atlasSize = 256.0F;
    const float u0 = 4.0F / atlasSize;
    const float v0 = 153.0F / atlasSize;
    const float u1 = 253.0F / atlasSize;
    const float v1 = 182.0F / atlasSize;
    constexpr float left = 65.0F;
    constexpr float top = 139.0F;
    constexpr float width = 350.0F;
    constexpr float height = 41.0F;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_resultsTexture);
    glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(left, top);
    glTexCoord2f(u1, v0); glVertex2f(left + width, top);
    glTexCoord2f(u1, v1); glVertex2f(left + width, top + height);
    glTexCoord2f(u0, v1); glVertex2f(left, top + height);
    glEnd();
    drawHudText("TOTAL " + std::to_string(g_playerPoints) + " P",
                190.0F, 188.0F, 1.25F);
}

void drawTitleOverlay() {
    drawTextureRegion(g_titleUiTexture, 0.0F, 0.0F, 373.0F, 51.0F,
                      53.0F, 62.0F, 373.0F, 51.0F);
    drawTextureRegion(g_menuUiTexture, 272.0F, 273.0F, 132.0F, 16.0F,
                      174.0F, 218.0F, 132.0F, 16.0F);
    drawTextureRegion(g_menuUiTexture, 273.0F, 289.0F, 118.0F, 24.0F,
                      181.0F, 271.0F, 118.0F, 24.0F);
}

void drawMainMenuOverlay() {
    drawTextureRegion(g_menuUiTexture, 18.0F, 143.0F, 247.0F, 236.0F,
                      28.0F, 36.0F, 247.0F, 236.0F);
    drawTextureRegion(g_menuUiTexture, 272.0F, 63.0F, 51.0F, 15.0F,
                      52.0F, 52.0F, 51.0F, 15.0F);
    constexpr std::array<const char*, 5> labels{{
        "CAMPAIGN", "CHALLENGE", "EXTRA", "SETTINGS", "TERMS"}};
    for (std::size_t index = 0; index < labels.size(); ++index) {
        const float y = 76.0F + static_cast<float>(index) * 32.0F;
        const float buttonSourceY = index == g_menuSelection ? 105.0F : 67.0F;
        drawTextureRegion(g_menuUiTexture, 0.0F, buttonSourceY, 154.0F, 31.0F,
                          47.0F, y, 166.0F, 29.0F);
        drawCenteredHudText(labels[index], 130.0F, y + 9.0F, 1.5F);
    }
}

void drawPopupButton(float x, float y, float width, float height,
                     const std::string& label, bool enabled = true,
                     bool selected = false) {
    drawTextureRegionSized(g_popupEnTexture, 256.0F, 256.0F,
                           121.0F, selected ? 202.0F : 178.0F,
                           118.0F, 22.0F, x, y, width, height);
    const float scale = label.size() > 14U ? 1.0F : 1.25F;
    const float textWidth = static_cast<float>(label.size()) * 6.0F * scale;
    drawHudText(label, x + (width - textWidth) * 0.5F,
                y + (height - 7.0F * scale) * 0.5F, scale);
    if (!enabled) {
        drawColoredRect(x + 2.0F, y + 2.0F, width - 4.0F, height - 4.0F,
                        0.0F, 0.0F, 0.02F, 0.62F);
    }
}

void drawSubmenuPanel(float x, float y, float width, float height) {
    drawTextureRegion(g_menuUiTexture, 18.0F, 143.0F, 247.0F, 236.0F,
                      x, y, width, height);
}

void drawChallengeOverlay() {
    drawMainMenuOverlay();
    drawColoredRect(25.0F, 30.0F, 270.0F, 250.0F,
                    0.0F, 0.0F, 0.03F, 0.28F);
    drawSubmenuPanel(52.0F, 40.0F, 225.0F, 236.0F);
    constexpr std::array<const char*, 6> labels{{
        "SURVIVAL", "INFINITE A", "INFINITE B",
        "INFINITE C", "INFINITE D", "INFINITE E"}};
    for (std::size_t index = 0; index < labels.size(); ++index) {
        drawPopupButton(76.0F, 59.0F + static_cast<float>(index) * 35.0F,
                        145.0F, 29.0F, labels[index], index == 0U, index == 0U);
    }
    drawPopupButton(52.0F, 278.0F, 72.0F, 29.0F, "BACK");
}

void drawExtraOverlay() {
    drawMainMenuOverlay();
    drawColoredRect(25.0F, 30.0F, 270.0F, 250.0F,
                    0.0F, 0.0F, 0.03F, 0.28F);
    drawSubmenuPanel(52.0F, 40.0F, 225.0F, 236.0F);
    drawHudText("EXTRA", 71.0F, 57.0F, 1.5F);
    constexpr std::array<const char*, 4> labels{{
        "RANKING", "ACHIEVEMENT", "INSTRUCTIONS", "WRITE A REVIEW"}};
    for (std::size_t index = 0; index < labels.size(); ++index) {
        drawPopupButton(76.0F, 83.0F + static_cast<float>(index) * 45.0F,
                        145.0F, 29.0F, labels[index]);
    }
    drawPopupButton(52.0F, 278.0F, 72.0F, 29.0F, "BACK");
}

void drawSettingsOverlay() {
    drawMainMenuOverlay();
    drawColoredRect(8.0F, 27.0F, 430.0F, 255.0F,
                    0.0F, 0.0F, 0.03F, 0.34F);
    drawSubmenuPanel(14.0F, 39.0F, 412.0F, 236.0F);
    constexpr std::array<const char*, 7> labels{{
        "CAMERA", "AIM ASSIST", "VIBRATION", "SOUND",
        "PRIORITY", "Y-AXIS", "SENSITIVITY"}};
    constexpr std::array<const char*, 6> leftLabels{{
        "EDGE PAN", "ON", "ON", "ON", "QUALITY", "NORMAL"}};
    constexpr std::array<const char*, 6> rightLabels{{
        "FIXED", "OFF", "OFF", "OFF", "SPEED", "REVERSED"}};
    const std::array<bool, 6> leftSelected{{
        g_settingTilt, g_settingMultiTouch, g_settingVibration,
        g_settingSound, g_settingQuality, !g_settingReverseY}};
    for (std::size_t index = 0; index < 6U; ++index) {
        const float y = 57.0F + static_cast<float>(index) * 30.0F;
        drawHudText(labels[index], 41.0F, y + 7.0F, 1.25F);
        drawPopupButton(137.0F, y, 98.0F, 23.0F,
                        leftLabels[index], true, leftSelected[index]);
        drawPopupButton(249.0F, y, 98.0F, 23.0F,
                        rightLabels[index], true, !leftSelected[index]);
        if (index == 0U || index == 4U || index == 5U) {
            drawPopupButton(354.0F, y, 50.0F, 23.0F, "TIP");
        }
    }
    drawHudText(labels[6], 41.0F, 244.0F, 1.25F);
    for (int index = 0; index < 10; ++index) {
        drawColoredRect(139.0F + static_cast<float>(index) * 19.0F, 244.0F,
                        16.0F, 10.0F,
                        index < g_settingSensitivity ? 0.85F : 0.08F,
                        index < g_settingSensitivity ? 0.9F : 0.1F,
                        index < g_settingSensitivity ? 1.0F : 0.18F, 0.95F);
    }
    drawPopupButton(354.0F, 239.0F, 50.0F, 23.0F, "TIP");
    drawPopupButton(14.0F, 278.0F, 72.0F, 29.0F, "BACK");
    drawPopupButton(286.0F, 278.0F, 140.0F, 29.0F, "CLEAR DATA");
}

void drawStageSelectOverlay() {
    drawTextureRegion(g_menuUiTexture, 18.0F, 143.0F, 247.0F, 236.0F,
                      15.0F, 37.0F, 410.0F, 236.0F);
    drawTextureRegion(g_menuUiTexture, 272.0F, 77.0F, 116.0F, 15.0F,
                      42.0F, 49.0F, 116.0F, 15.0F);
    for (std::size_t row = 0; row < 5U; ++row) {
        for (std::size_t column = 0; column < 7U; ++column) {
            const bool selected = row == g_stageRow && column == g_stageColumn;
            const bool available = column < 5U;
            const float x = 39.0F + static_cast<float>(column) * 56.0F;
            const float y = 76.0F + static_cast<float>(row) * 34.0F;
            drawTextureRegion(g_menuUiTexture, 156.0F,
                              selected ? 105.0F : 67.0F, 60.0F, 31.0F,
                              x, y, 52.0F, 27.0F);
            std::string label;
            if (column < 5U) {
                label = std::to_string(column + 1U) + "-" + std::to_string(row + 1U);
            } else {
                const std::size_t extra = row + 1U + (column == 6U ? 5U : 0U);
                label = "EX" + std::to_string(extra);
            }
            drawHudText(label, x + 6.0F, y + 7.0F, 1.5F);
            if (column < 5U) {
                const std::size_t missionIndex = column * 5U + row;
                if (g_clearedMissions[missionIndex]) {
                    drawHudText("C", x + 40.0F, y + 3.0F, 0.8F);
                }
            }
            if (!available) {
                drawColoredRect(x + 2.0F, y + 2.0F, 48.0F, 23.0F,
                                0.0F, 0.0F, 0.02F, 0.58F);
            }
        }
    }
    drawHudText("NORMAL", 55.0F, 15.0F, 1.5F);
    drawHudText("HARD", 218.0F, 15.0F, 1.5F);
    drawHudText("EXTREME", 338.0F, 15.0F, 1.5F);
    drawTextureRegion(g_menuUiTexture, 0.0F, 385.0F, 108.0F, 22.0F,
                      16.0F, 281.0F, 82.0F, 23.0F);
    drawHudText("BACK", 33.0F, 286.0F, 1.5F);
}

void drawStoryOverlay() {
    const GLuint storyTexture = g_storyTextures[std::min<std::size_t>(
        g_selectedStageTheme, g_storyTextures.size() - 1U)];
    drawTextureRegion(storyTexture, 0.0F, 0.0F, 480.0F, 320.0F,
                      0.0F, 0.0F, kLogicalWidth, kLogicalHeight);
    drawTextureRegion(storyTexture, 0.0F, 328.0F, 480.0F, 111.0F,
                      0.0F, 136.0F, 480.0F, 111.0F);
    drawTextureRegion(storyTexture, 166.0F, 493.0F, 132.0F, 16.0F,
                      166.0F, 301.0F, 132.0F, 16.0F);
}

void drawArmoryOverlay() {
    drawTextureRegion(g_armoryUi1Texture, 0.0F, 0.0F, 480.0F, 320.0F,
                      0.0F, 0.0F, kLogicalWidth, kLogicalHeight);
    constexpr std::array<float, 3> columns{{8.0F, 151.0F, 294.0F}};
    constexpr std::array<float, 3> rows{{38.0F, 114.0F, 199.0F}};
    const auto slotTone = [](std::size_t slot) {
        return slot == g_armorySelection ? 1.0F : 0.025F;
    };
    constexpr std::array<std::array<float, 3>, 3> panelTint{{
        {{0.82F, 0.95F, 0.82F}},
        {{0.72F, 0.78F, 1.0F}},
        {{1.0F, 0.72F, 0.88F}},
    }};
    for (std::size_t row = 0; row < rows.size(); ++row) {
        for (std::size_t column = 0; column < columns.size(); ++column) {
            drawTextureRegionTinted(g_armoryUi3Texture,
                                    0.0F, 0.0F, 458.0F, 228.0F,
                                    columns[column], rows[row], 139.0F, 72.0F,
                                    panelTint[column][0],
                                    panelTint[column][1],
                                    panelTint[column][2]);
            const std::size_t index = row * 3U + column;
            if (index == g_armorySelection) {
                drawColoredRect(columns[column] + 3.0F, rows[row] + 3.0F,
                                133.0F, 66.0F, 0.75F, 0.85F, 1.0F, 0.16F);
            }
        }
    }
    drawColoredRect(10.0F, 39.0F, 135.0F, 11.0F, 0.9F, 1.0F, 0.1F, 0.95F);
    drawHudText("ARMED PRIMARY", 36.0F, 41.0F, 1.0F);
    drawHudText("MACHINE GUN TYPE", 34.0F, 30.0F, 1.0F);
    drawHudText("OPTICAL TYPE", 180.0F, 30.0F, 1.0F);
    drawHudText("MISSILE TYPE", 337.0F, 30.0F, 1.0F);
    drawHudText("SUPPORT EQUIPMENTS", 177.0F, 189.0F, 1.0F);

    drawTextureRegionTinted(g_armoryUi2Texture, 8.0F, 20.0F, 215.0F, 83.0F,
                            30.0F, 51.0F, 104.0F, 48.0F,
                            slotTone(0U), slotTone(0U), slotTone(0U));
    drawTextureRegionTinted(g_armoryUi2Texture, 276.0F, 3.0F, 122.0F, 103.0F,
                            184.0F, 44.0F, 77.0F, 61.0F,
                            slotTone(1U), slotTone(1U), slotTone(1U));
    drawTextureRegionTinted(g_armoryUi2Texture, 52.0F, 125.0F, 127.0F, 117.0F,
                            326.0F, 43.0F, 82.0F, 62.0F,
                            slotTone(2U), slotTone(2U), slotTone(2U));
    drawTextureRegionTinted(g_armoryUi2Texture, 253.0F, 129.0F, 229.0F, 111.0F,
                            25.0F, 126.0F, 108.0F, 52.0F,
                            slotTone(3U), slotTone(3U), slotTone(3U));
    drawTextureRegionTinted(g_armoryUi2Texture, 270.0F, 250.0F, 130.0F, 100.0F,
                            181.0F, 124.0F, 88.0F, 55.0F,
                            slotTone(4U), slotTone(4U), slotTone(4U));
    drawTextureRegionTinted(g_armoryUi2Texture, 259.0F, 381.0F, 220.0F, 86.0F,
                            312.0F, 128.0F, 110.0F, 48.0F,
                            slotTone(5U), slotTone(5U), slotTone(5U));
    drawTextureRegionTinted(g_armoryUi2Texture, 54.0F, 383.0F, 115.0F, 103.0F,
                            36.0F, 210.0F, 78.0F, 55.0F,
                            slotTone(6U), slotTone(6U), slotTone(6U));
    drawTextureRegion(g_armoryUi1Texture, 305.0F, 397.0F, 175.0F, 100.0F,
                      185.0F, 208.0F, 103.0F, 59.0F);
    drawTextureRegionTinted(g_armoryUi2Texture, 270.0F, 250.0F, 130.0F, 100.0F,
                            331.0F, 210.0F, 75.0F, 55.0F,
                            slotTone(8U), slotTone(8U), slotTone(8U));
    drawHudText(kWeaponSpecs[g_equippedWeapon].label, 153.0F, 274.0F, 1.0F);
    drawHudText("LV " + std::to_string(g_weaponLevels[g_equippedWeapon] + 1) +
                "/6  DMG " + std::to_string(weaponDamage(g_equippedWeapon)),
                245.0F, 274.0F, 0.9F);
    const int upgradeCost = weaponUpgradeCost(g_equippedWeapon);
    drawHudText(upgradeCost == 0 ? "MAX LEVEL" :
                "UPGRADE " + std::to_string(upgradeCost) + " P",
                178.0F, 259.0F, 1.15F);

    drawTextureRegionSized(g_popupEnTexture, 256.0F, 256.0F,
                           121.0F, 178.0F, 118.0F, 22.0F,
                           2.0F, 289.0F, 140.0F, 29.0F);
    drawTextureRegionSized(g_popupEnTexture, 256.0F, 256.0F,
                           121.0F, 178.0F, 118.0F, 22.0F,
                           338.0F, 289.0F, 140.0F, 29.0F);
    drawHudText("BACK", 51.0F, 298.0F, 1.5F);
    drawHudText("YOU HAVE " + std::to_string(g_playerPoints) + " P",
                184.0F, 298.0F, 1.25F);
    drawHudText("START", 382.0F, 298.0F, 1.5F);
}

void drawArmoryConfirmOverlay() {
    drawArmoryOverlay();
    drawColoredRect(0.0F, 0.0F, kLogicalWidth, kLogicalHeight,
                    0.0F, 0.0F, 0.02F, 0.38F);
    drawTextureRegionSized(g_popupEnTexture, 256.0F, 256.0F,
                           0.0F, 0.0F, 240.0F, 128.0F,
                           105.0F, 76.0F, 236.0F, 140.0F);
    drawCenteredHudText("START MISSION WITH CURRENT", 223.0F, 113.0F, 1.0F);
    drawCenteredHudText("SETTING?", 223.0F, 128.0F, 1.0F);
    drawPopupButton(112.0F, 187.0F, 112.0F, 28.0F, "CANCEL");
    drawPopupButton(244.0F, 187.0F, 112.0F, 28.0F, "OK", true, true);
    if (kWeaponSpecs[g_equippedWeapon].behavior == WeaponBehavior::Shield &&
        selectedMissionSpec().objective != MissionObjective::Survive) {
        drawHudText("SUPPORT ONLY - SELECT AN OFFENSIVE WEAPON",
                    115.0F, 228.0F, 0.9F);
    }
}

void drawMissionFailedOverlay() {
    drawLogicalTexture(g_gameoverTexture, 0.0F, 0.0F,
                       kLogicalWidth, kLogicalHeight);
    drawTextureRegionSized(g_popupEnTexture, 256.0F, 256.0F,
                           121.0F, 178.0F, 118.0F, 22.0F,
                           2.0F, 286.0F, 140.0F, 31.0F);
    drawTextureRegionSized(g_popupEnTexture, 256.0F, 256.0F,
                           121.0F, 178.0F, 118.0F, 22.0F,
                           338.0F, 286.0F, 140.0F, 31.0F);
    drawHudText("BACK", 51.0F, 296.0F, 1.5F);
    drawHudText("RETRY", 382.0F, 296.0F, 1.5F);
}

void drawGameplayAimAndShot() {
    const float aimX = g_aimX;
    const float aimY = g_aimY;
    if (g_shotActive && g_gunFireLoaded) {
        const auto& animationMotion = g_gunFireAnimation.motions.front();
        const auto& descriptor = g_gunFireAction.motions[animationMotion.motionIndex];
        const auto elapsed = GetTickCount64() - g_shotStartTime;
        const auto duration = static_cast<ULONGLONG>(descriptor.frameCount) * 1000ULL /
            descriptor.framesPerSecond;
        if (elapsed >= duration) {
            g_shotActive = false;
        } else {
            const std::size_t frame = std::min<std::size_t>(
                elapsed * descriptor.framesPerSecond / 1000ULL,
                descriptor.frameCount - 1U);
            const auto& track = animationMotion.tracks.front();
            const auto& key = track.keys[track.frameToKey[frame]];
            const float offsetU = static_cast<float>(key.u) / 65536.0F;
            const float offsetV = static_cast<float>(key.v) / 65536.0F;
            const auto& minimum = g_gunFireMesh.bounds[0];
            const auto& maximum = g_gunFireMesh.bounds[1];
            const float extentX = std::max(maximum.x - minimum.x, 0.001F);
            const float extentY = std::max(maximum.y - minimum.y, 0.001F);
            constexpr float effectSize = 72.0F;
            glEnable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glBindTexture(GL_TEXTURE_2D, g_gunFireTexture);
            glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
            glBegin(GL_TRIANGLES);
            for (const std::uint16_t index : g_gunFireMesh.indices) {
                const auto& uv = g_gunFireMesh.texcoords[index];
                const auto& position = g_gunFireMesh.positions[index];
                const float x = aimX - effectSize * 0.5F +
                    (position.x - minimum.x) / extentX * effectSize;
                const float y = aimY - effectSize * 0.5F +
                    (position.y - minimum.y) / extentY * effectSize;
                glTexCoord2f(uv.x + offsetU, uv.y + offsetV);
                glVertex2f(x, y);
            }
            glEnd();
        }
    }
    const int targetTextureIndex = kWeaponTargetTexture[g_equippedWeapon];
    if (targetTextureIndex >= 0) {
        const float size = kWeaponTargetSizes[g_equippedWeapon];
        drawLogicalTexture(
            g_targetTextures[static_cast<std::size_t>(targetTextureIndex)],
            aimX - size * 0.5F, aimY - size * 0.5F, size, size);
    }
    if (g_leftTriggerHeld) {
        drawColoredRect(aimX - 28.0F, aimY - 8.0F, 3.0F, 16.0F,
                        0.15F, 0.85F, 1.0F, 0.95F);
    }
    if (g_rightTriggerHeld) {
        drawColoredRect(aimX + 25.0F, aimY - 8.0F, 3.0F, 16.0F,
                        1.0F, 0.48F, 0.12F, 0.95F);
    }
}

float missionElapsedSeconds() {
    if (g_missionStartTime == 0) {
        return 0.0F;
    }
    const ULONGLONG endTime =
        g_runtimeState == RuntimeState::Paused && g_pauseStartTime != 0
            ? g_pauseStartTime : GetTickCount64();
    return static_cast<float>(endTime - g_missionStartTime) / 1000.0F;
}

void finishMission() {
    if (g_missionComplete) {
        return;
    }
    g_missionComplete = true;
    g_leftTriggerHeld = false;
    g_rightTriggerHeld = false;
    int reward = 0;
    if (g_challengeMode) {
        reward = 250;
    } else {
        const std::size_t missionIndex = g_selectedStageTheme * 5U + g_stageRow;
        if (missionIndex < g_clearedMissions.size() &&
            !g_clearedMissions[missionIndex]) {
            g_clearedMissions[missionIndex] = true;
            reward = std::clamp(
                g_stageTable.signedValue(missionIndex, 4U, 100), 100, 50000);
        }
    }
    if (reward > 0) {
        g_playerPoints = std::min(9999999, g_playerPoints + reward);
        saveProgress();
    }
    stopBgm();
    if (g_settingSound) {
        const auto completeSound =
            executableDirectory() / L"assets" / L"mission-complete.wav";
        PlaySoundW(completeSound.c_str(), nullptr,
                   SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
    std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
    diagnostics << "missionReward=" << reward
                << ",playerPoints=" << g_playerPoints << '\n';
}

void updateMissionObjective() {
    if (g_runtimeState != RuntimeState::Playing ||
        g_missionComplete || g_missionFailed) {
        return;
    }
    const MissionSpec& mission = selectedMissionSpec();
    if (missionElapsedSeconds() < static_cast<float>(mission.durationSeconds)) {
        return;
    }
    if (mission.objective == MissionObjective::Survive) {
        finishMission();
    } else if (g_targetsRemaining > 0) {
        g_missionFailed = true;
        g_leftTriggerHeld = false;
        g_rightTriggerHeld = false;
        stopBgm();
    }
}

void updateMissionDamage() {
    if (g_runtimeState != RuntimeState::Playing || g_missionComplete || g_enemies.empty()) {
        return;
    }
    const float seconds = missionElapsedSeconds();
    const PatternRuntime pattern = selectedPatternRuntime();
    const int cycle = static_cast<int>(seconds / pattern.cycleSeconds);
    const float phase = std::fmod(seconds, pattern.cycleSeconds);
    const std::size_t activeCount = std::clamp<std::size_t>(
        selectedMissionSpec().maxActiveEnemies, 1U, g_enemies.size());
    const std::size_t attacker = static_cast<std::size_t>(cycle) % activeCount;
    if (phase < pattern.cycleSeconds * 0.67F ||
        phase > pattern.cycleSeconds * 0.78F || cycle == g_lastDamageCycle ||
        !g_enemies[attacker].alive) {
        return;
    }
    g_lastDamageCycle = cycle;
    g_playerHitEffectStart = GetTickCount64();
    const float hitStrength = 0.10F +
        static_cast<float>(pattern.attackMode) * 0.025F;
    if (g_playerShield > 0.0F) {
        g_playerShield = std::max(0.0F, g_playerShield - hitStrength);
    } else {
        g_playerHull = std::max(0.0F, g_playerHull - hitStrength * 1.25F);
    }
    if (g_playerHull <= 0.0F) {
        g_missionFailed = true;
        g_leftTriggerHeld = false;
        g_rightTriggerHeld = false;
        stopBgm();
    }
    std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
    diagnostics << "damageCycle=" << cycle
                << ",shield=" << g_playerShield
                << ",hull=" << g_playerHull << '\n';
}

void updateEnemyWaves() {
    if (g_runtimeState != RuntimeState::Playing || g_missionComplete ||
        g_targetsRemaining <= 0) {
        return;
    }
    const auto aliveCount = static_cast<int>(std::count_if(
        g_enemies.begin(), g_enemies.end(),
        [](const EnemyInstance& enemy) { return enemy.alive; }));
    int availableSlots = std::max(0, g_targetsRemaining - aliveCount);
    const ULONGLONG now = GetTickCount64();
    for (auto& enemy : g_enemies) {
        if (availableSlots <= 0) {
            break;
        }
        if (!enemy.alive && enemy.respawnAt != 0 && now >= enemy.respawnAt) {
            enemy.alive = true;
            enemy.respawnAt = 0;
            enemy.hitPoints = selectedMissionSpec().enemyHitPoints;
            --availableSlots;
        }
    }
}

void drawMissionHud() {
    drawTextureRegion(g_cockpitTexture, 0.0F, 10.0F, 480.0F, 135.0F,
                      0.0F, 215.0F, 480.0F, 135.0F);
    drawTextureRegion(g_cockpitTexture, 14.0F, 142.0F, 132.0F, 132.0F,
                      3.0F, 177.0F, 124.0F, 124.0F, 0.82F);
    drawTextureRegion(g_cockpitTexture, 14.0F, 142.0F, 132.0F, 132.0F,
                      353.0F, 177.0F, 124.0F, 124.0F, 0.82F);
    drawColoredRect(80.0F, 307.0F, 90.0F * g_playerShield, 7.0F,
                    0.15F, 0.78F, 1.0F, 0.92F);
    drawColoredRect(310.0F, 307.0F, 90.0F * g_playerHull, 7.0F,
                    1.0F, 0.25F, 0.18F, 0.92F);
    drawTextureRegion(g_cockpitTexture, 408.0F, 158.0F, 31.0F, 31.0F,
                      2.0F, 2.0F, 27.0F, 27.0F);
    const MissionSpec& mission = selectedMissionSpec();
    if (mission.objective == MissionObjective::Survive) {
        const int secondsRemaining = std::max(
            0, mission.durationSeconds - static_cast<int>(missionElapsedSeconds()));
        drawHudText("TIME: " + std::to_string(secondsRemaining), 4.0F, 57.0F, 2.0F);
    } else {
        drawHudText("TARGET: " + std::to_string(g_targetsRemaining), 4.0F, 57.0F, 2.0F);
    }
    float headingDegrees = std::fmod(g_missionYawDegrees +
        g_cameraYawOffsetDegrees, 360.0F);
    if (headingDegrees < 0.0F) headingDegrees += 360.0F;
    const char* heading = headingDegrees < 22.5F || headingDegrees >= 337.5F ? "N" :
        (headingDegrees < 67.5F ? "NE" :
        (headingDegrees < 112.5F ? "E" :
        (headingDegrees < 157.5F ? "SE" :
        (headingDegrees < 202.5F ? "S" :
        (headingDegrees < 247.5F ? "SW" :
        (headingDegrees < 292.5F ? "W" : "NW"))))));
    drawHudText(heading, heading[1] == '\0' ? 237.0F : 232.0F, 34.0F, 1.5F);
    drawColoredRect(54.0F, 112.0F, 67.0F, 12.0F, 0.01F, 0.02F, 0.09F, 0.92F);
    drawColoredRect(359.0F, 112.0F, 67.0F, 12.0F, 0.01F, 0.02F, 0.09F, 0.92F);
    drawHudText("LMB", 56.0F, 98.0F, 1.0F);
    drawHudText("RMB", 400.0F, 98.0F, 1.0F);
    drawHudText(g_leftReloading ? "RLD" : std::to_string(g_leftAmmo),
                g_leftReloading ? 72.0F : 78.0F, 114.0F, 1.25F);
    drawHudText(g_rightReloading ? "RLD" : std::to_string(g_rightAmmo),
                g_rightReloading ? 377.0F : 383.0F, 114.0F, 1.25F);
}

void drawCombatEffects() {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = now - g_combatEffectStart;
    const ULONGLONG duration = combatEffectDurationMs(g_combatEffect);
    if (g_combatEffect != CombatEffect::None && elapsed < duration) {
        const float fade = 1.0F - static_cast<float>(elapsed) /
            static_cast<float>(duration);
        switch (g_combatEffect) {
        case CombatEffect::Hit:
            drawLogicalTexture(g_hitEffectTextures[g_hitEffectVariant % 4U],
                g_combatEffectX - 38.0F, g_combatEffectY - 38.0F,
                76.0F, 76.0F, fade);
            break;
        case CombatEffect::EnemyDie:
        case CombatEffect::MissileExplosion: {
            const GLuint texture = g_combatEffect == CombatEffect::EnemyDie
                ? g_enemyDieTexture : g_missileExplosionTexture;
            const std::size_t frame = combatEffectSpriteFrame(
                g_combatEffect, elapsed, 4U);
            const float size = g_combatEffect == CombatEffect::EnemyDie ? 118.0F : 104.0F;
            drawTextureRegionSized(texture, 256.0F, 256.0F,
                static_cast<float>(frame % 2U) * 128.0F,
                static_cast<float>(frame / 2U) * 128.0F, 128.0F, 128.0F,
                g_combatEffectX - size * 0.5F, g_combatEffectY - size * 0.5F,
                size, size, fade);
            break;
        }
        case CombatEffect::Emp:
            drawLogicalTexture(g_empFieldTextures[(elapsed / 90ULL) % 2ULL],
                85.0F, 5.0F, 310.0F, 310.0F, fade * 0.85F);
            break;
        case CombatEffect::Shield:
            drawLogicalTexture(g_shieldFieldTexture,
                157.0F, 77.0F, 166.0F, 166.0F, fade * 0.9F);
            break;
        case CombatEffect::Thunder:
            drawLogicalTexture(g_thunderFireTexture,
                g_combatEffectX - 72.0F, g_combatEffectY - 72.0F,
                144.0F, 144.0F, fade);
            break;
        default:
            break;
        }
    }
    const MissionSpec& mission = selectedMissionSpec();
    if (mission.objective == MissionObjective::Boss && !g_missionComplete) {
        const PatternRuntime pattern = selectedPatternRuntime();
        const float phase = std::fmod(missionElapsedSeconds(), pattern.cycleSeconds);
        const float laserStart = pattern.cycleSeconds * 0.58F;
        const float peftDuration = g_bossLaserEffectDescriptorLoaded &&
            !g_bossLaserEffectDescriptor.motions.empty()
            ? static_cast<float>(g_bossLaserEffectDescriptor.motions.front().frameCount) /
                g_bossLaserEffectDescriptor.motions.front().framesPerSecond
            : pattern.cycleSeconds * 0.20F;
        const float laserEnd = std::min(pattern.cycleSeconds,
                                        laserStart + peftDuration);
        if (phase >= laserStart && phase <= laserEnd) {
            const float pulse = 0.65F + 0.25F *
                std::sin(missionElapsedSeconds() * 21.0F);
            drawLogicalTexture(g_bossLaserTexture, 176.0F, 78.0F,
                               128.0F, 178.0F, pulse);
        }
    }
}

void drawDamageOverlay() {
    const ULONGLONG elapsed = GetTickCount64() - g_playerHitEffectStart;
    if (g_playerHitEffectStart == 0 || elapsed >= 520ULL) return;
    const float fade = 1.0F - static_cast<float>(elapsed) / 520.0F;
    drawLogicalTexture(g_playerShield > 0.0F ?
        g_playerShieldHitTexture : g_playerHitTexture,
        0.0F, 0.0F, kLogicalWidth, kLogicalHeight, fade * 0.72F);
    drawTextureRegion(g_cockpitTexture, 321.0F, 194.0F, 93.0F, 32.0F,
                      194.0F, 253.0F, 93.0F, 32.0F);
    drawTextureRegion(g_cockpitTexture, 321.0F, 194.0F, 93.0F, 32.0F,
                      5.0F, 31.0F, 47.0F, 16.0F);
}

void drawPauseOverlay() {
    drawColoredRect(0.0F, 0.0F, kLogicalWidth, kLogicalHeight,
                    0.0F, 0.0F, 0.05F, 0.32F);
    for (const float y : {70.0F, 115.0F, 158.0F}) {
        drawTextureRegionSized(g_pauseTexture, 256.0F, 256.0F,
                               128.0F, 178.0F, 126.0F, 22.0F,
                               130.0F, y, 220.0F, 38.0F);
    }
    drawHudText("CONTINUE", 206.0F, 83.0F, 1.4F);
    drawHudText("CALIBRATE", 202.0F, 128.0F, 1.4F);
    drawHudText("ABORT MISSION", 185.0F, 171.0F, 1.4F);
}

void drawMissionScene3d() {
    const float aspect = kLogicalWidth / kLogicalHeight;
    constexpr float pi = 3.14159265358979323846F;
    const float yawDegrees = g_missionYawDegrees + g_cameraYawOffsetDegrees;
    const float yaw = yawDegrees * pi / 180.0F;
    const float pitch = g_cameraPitchDegrees * pi / 180.0F;
    const pact::Vec3 cameraPosition{0.0F, 24.0F, 0.0F};
    const pact::Vec3 cameraTarget{cameraPosition.x + std::sin(yaw) * 100.0F,
                                  cameraPosition.y + 18.0F + std::tan(pitch) * 100.0F,
                                  cameraPosition.z - std::cos(yaw) * 100.0F};
    glClearColor(0.015F, 0.02F, 0.025F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    applyPerspective(55.0F, aspect, 1.0F, 1400.0F);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    applyLookAt(cameraPosition, cameraTarget, 0.0F);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
    const auto drawPart = [](const ScenePart& part) {
        glBindTexture(GL_TEXTURE_2D, part.texture);
        glBegin(GL_TRIANGLES);
        for (const std::uint16_t index : part.mesh.indices) {
            const auto& uv = part.mesh.texcoords[index];
            const auto& position = part.mesh.positions[index];
            glTexCoord2f(uv.x * part.uvScaleU, uv.y * part.uvScaleV);
            glVertex3f(position.x, position.y, position.z);
        }
        glEnd();
    };
    const auto drawBoneAnimation =
        [](const ScenePart& part, const EnemyVertexAnimation& animation,
           float elapsedSeconds) {
            if (!animation.boneLoaded || animation.boneFrames.empty() ||
                animation.boneSection.motions.empty()) {
                return false;
            }
            const auto& motion = animation.boneSection.motions.front();
            const auto& descriptor = animation.action.motions[motion.motionIndex];
            const float framePosition =
                elapsedSeconds * static_cast<float>(descriptor.framesPerSecond);
            const auto baseFrame = static_cast<std::size_t>(
                std::floor(framePosition)) % animation.boneFrames.size();
            const auto nextFrame = (baseFrame + 1U) % animation.boneFrames.size();
            const float fraction = framePosition - std::floor(framePosition);
            glBindTexture(GL_TEXTURE_2D, part.texture);
            glBegin(GL_TRIANGLES);
            for (const std::uint16_t index : part.mesh.indices) {
                const auto& uv = part.mesh.texcoords[index];
                const auto& from = animation.boneFrames[baseFrame][index];
                const auto& to = animation.boneFrames[nextFrame][index];
                glTexCoord2f(uv.x * part.uvScaleU, uv.y * part.uvScaleV);
                glVertex3f(from.x + (to.x - from.x) * fraction,
                           from.y + (to.y - from.y) * fraction,
                           from.z + (to.z - from.z) * fraction);
            }
            glEnd();
            return true;
        };
    const auto drawVertexAnimation =
        [&](const ScenePart& part, const EnemyVertexAnimation& animation,
            float elapsedSeconds) {
            if (!animation.loaded || animation.section.motions.empty()) {
                return;
            }
            const auto& motion = animation.section.motions.front();
            if (motion.frames.empty()) {
                return;
            }
            const auto& descriptor =
                animation.action.motions[motion.motionIndex];
            const float framePosition =
                elapsedSeconds * static_cast<float>(descriptor.framesPerSecond);
            const auto baseFrame = static_cast<std::size_t>(
                std::floor(framePosition)) % motion.frames.size();
            const auto nextFrame = (baseFrame + 1U) % motion.frames.size();
            const float fraction = framePosition - std::floor(framePosition);
            glBindTexture(GL_TEXTURE_2D, part.texture);
            glBegin(GL_TRIANGLES);
            const std::size_t remapEnd =
                static_cast<std::size_t>(motion.remapOffset) +
                motion.remapCount;
            for (std::size_t remapIndex = motion.remapOffset;
                 remapIndex < remapEnd; ++remapIndex) {
                const std::size_t vertex =
                    animation.section.remapIndices[remapIndex];
                const auto& uv = animation.section.texcoords[
                    static_cast<std::size_t>(motion.texcoordOffset) + vertex];
                const auto& from = motion.frames[baseFrame][vertex];
                const auto& to = motion.frames[nextFrame][vertex];
                glTexCoord2f(uv.x * part.uvScaleU, uv.y * part.uvScaleV);
                glVertex3f(
                    from.x + (to.x - from.x) * fraction,
                    from.y + (to.y - from.y) * fraction,
                    from.z + (to.z - from.z) * fraction);
            }
            glEnd();
        };
    const auto drawEnvironment = [&](const auto& scene) {
        for (const auto& part : scene) {
            drawPart(part);
        }
    };
    switch (g_selectedStageTheme) {
    case 1U:
        drawEnvironment(g_missionScene2);
        break;
    case 2U:
        drawEnvironment(g_missionScene3);
        break;
    case 3U:
        drawEnvironment(g_missionScene4);
        break;
    case 4U:
        drawEnvironment(g_missionScene5);
        break;
    default:
        drawEnvironment(g_missionScene);
        break;
    }
    const float missionSeconds = missionElapsedSeconds();
    const MissionSpec& mission = selectedMissionSpec();
    const PatternRuntime pattern = selectedPatternRuntime();
    constexpr float radiansToDegrees = 57.295779513082320876F;
    for (std::size_t index = 0; index < g_enemies.size(); ++index) {
        if (!g_enemies[index].alive) {
            continue;
        }
        auto& enemy = g_enemies[index];
        pact::Vec3 position = enemy.position;
        pact::Vec3 velocity{};
        if (pattern.movementType == 1 && mission.patternId == 1U) {
            const OriginalEnemyMotion motion =
                originalPattern1Motion(index, missionSeconds, pattern);
            position = motion.position;
            velocity = motion.velocity;
        } else if (pattern.movementType == 2 && mission.patternId == 5U) {
            const OriginalEnemyMotion motion =
                originalPattern5Motion(index, missionSeconds, pattern);
            position = motion.position;
            velocity = motion.velocity;
        }
        constexpr float projectionScale = 307.4F;
        const float cosine = std::cos(yaw);
        const float sine = std::sin(yaw);
        const float relativeY = position.y - cameraPosition.y;
        const float cameraX = position.x * cosine + position.z * sine;
        const float depth = position.x * sine - position.z * cosine;
        if (depth > 1.0F) {
            const float cameraY = relativeY -
                (0.18F + std::tan(pitch)) * depth;
            enemy.targetX = kLogicalWidth * 0.5F +
                cameraX * projectionScale / depth;
            enemy.targetY = kLogicalHeight * 0.5F -
                cameraY * projectionScale / depth;
        } else {
            enemy.targetX = -1000.0F;
            enemy.targetY = -1000.0F;
        }
        glPushMatrix();
        glTranslatef(position.x, position.y, position.z);
        const float horizontalSpeed = std::sqrt(
            velocity.x * velocity.x + velocity.z * velocity.z);
        const float totalSpeed = std::sqrt(
            horizontalSpeed * horizontalSpeed + velocity.y * velocity.y);
        if (totalSpeed > 0.001F) {
            // The model orientation follows the recovered trajectory derivative.
            // This replaces the port's arbitrary perpetual spin.
            const float heading = std::atan2(velocity.x, -velocity.z) * radiansToDegrees;
            const float pitchDegrees =
                std::atan2(velocity.y, std::max(0.001F, horizontalSpeed)) *
                radiansToDegrees;
            const float bankDegrees = -std::atan2(
                velocity.x,
                std::max(0.001F, std::sqrt(
                    velocity.y * velocity.y + velocity.z * velocity.z))) *
                radiansToDegrees;
            glRotatef(heading, 0.0F, 1.0F, 0.0F);
            glRotatef(pitchDegrees, 1.0F, 0.0F, 0.0F);
            glRotatef(bankDegrees, 0.0F, 0.0F, 1.0F);
        } else {
            glRotatef(180.0F, 0.0F, 1.0F, 0.0F);
        }
        const float scale = 0.72F * kEnemyModelScales[mission.enemyModel];
        glScalef(scale, scale, scale);
        const auto& enemyPart = g_enemySceneParts[mission.enemyModel];
        const auto& enemyAnimation = g_enemyVertexAnimations[mission.enemyModel];
        if (!drawBoneAnimation(enemyPart, enemyAnimation, missionSeconds)) {
            drawPart(enemyPart);
        }
        drawVertexAnimation(enemyPart, enemyAnimation, missionSeconds);
        glPopMatrix();
    }

    glClear(GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glPushMatrix();
    glTranslatef(-11.5F, -3.8F, -13.5F);
    glRotatef(-8.0F, 0.0F, 1.0F, 0.0F);
    const float weaponScale = 1.15F * kWeaponModelScales[g_equippedWeapon];
    glScalef(weaponScale, weaponScale, weaponScale);
    drawPart(g_weaponSceneParts[g_equippedWeapon]);
    glPopMatrix();
    glPushMatrix();
    glTranslatef(11.5F, -3.8F, -13.5F);
    glRotatef(8.0F, 0.0F, 1.0F, 0.0F);
    glScalef(-weaponScale, weaponScale, weaponScale);
    drawPart(g_weaponSceneParts[g_equippedWeapon]);
    glPopMatrix();
}

void render() {
    if (!g_deviceContext || !g_glContext) {
        return;
    }

    glViewport(0, 0, std::max(g_clientWidth, 1), std::max(g_clientHeight, 1));
    glClearColor(0.015f, 0.025f, 0.045f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const RECT viewport = logicalViewport();
    glViewport(viewport.left, g_clientHeight - viewport.bottom,
        viewport.right - viewport.left, viewport.bottom - viewport.top);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, kLogicalWidth, kLogicalHeight, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    if (g_fireMode && g_gunFireLoaded) {
        const auto& animationMotion = g_gunFireAnimation.motions.front();
        const auto& descriptor = g_gunFireAction.motions[animationMotion.motionIndex];
        const auto& track = animationMotion.tracks.front();
        const ULONGLONG elapsed = GetTickCount64() - g_gunFireStartTime;
        const std::size_t frame = static_cast<std::size_t>(
            (elapsed * descriptor.framesPerSecond / 1000ULL) % descriptor.frameCount);
        if (frame < 32U) {
            g_fireObservedFrames |= 1U << frame;
        }
        const std::size_t keyIndex = track.frameToKey[frame];
        const auto& key = track.keys[keyIndex];
        const float offsetU = static_cast<float>(key.u) / 65536.0f;
        const float offsetV = static_cast<float>(key.v) / 65536.0f;
        const float aspect = kLogicalWidth / kLogicalHeight;
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(-1.2 * aspect, 1.2 * aspect, -1.2, 1.2, -10.0, 10.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        const auto& minimum = g_gunFireMesh.bounds[0];
        const auto& maximum = g_gunFireMesh.bounds[1];
        const float centerX = (minimum.x + maximum.x) * 0.5f;
        const float centerY = (minimum.y + maximum.y) * 0.5f;
        const float centerZ = (minimum.z + maximum.z) * 0.5f;
        const float extent = std::max({maximum.x - minimum.x, maximum.y - minimum.y,
                                       maximum.z - minimum.z, 0.001f});
        glScalef(1.8f / extent, 1.8f / extent, 1.8f / extent);
        glTranslatef(-centerX, -centerY, -centerZ);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_TEXTURE_2D);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glBindTexture(GL_TEXTURE_2D, g_gunFireTexture);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_TRIANGLES);
        for (const std::uint16_t index : g_gunFireMesh.indices) {
            const auto& uv = g_gunFireMesh.texcoords[index];
            const auto& position = g_gunFireMesh.positions[index];
            glTexCoord2f(uv.x + offsetU, uv.y + offsetV);
            glVertex3f(position.x, position.y, position.z);
        }
        glEnd();
        if (!g_fireFrameLogged) {
            const GLenum error = glGetError();
            std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
            diagnostics << "gunFireFrameGlError=" << static_cast<unsigned int>(error) << '\n'
                        << "gunFireFirstFrame=" << frame << '\n'
                        << "gunFireFirstKey=" << keyIndex << '\n'
                        << "gunFireFirstUvOffset=" << offsetU << ',' << offsetV << '\n';
            g_fireFrameLogged = true;
        }
        if (!g_fireLoopLogged && descriptor.frameCount < 32U &&
            g_fireObservedFrames == ((1U << descriptor.frameCount) - 1U)) {
            const GLenum error = glGetError();
            std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
            diagnostics << "gunFireFramesObserved=" << descriptor.frameCount << '\n'
                        << "gunFireLoopGlError=" << static_cast<unsigned int>(error) << '\n';
            g_fireLoopLogged = true;
        }
        if (g_captureRequested && !g_captureWritten) {
            g_captureWritten = captureViewportBmp(viewport);
            std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
            diagnostics << "captureWritten=" << (g_captureWritten ? 1 : 0) << '\n'
                        << "captureAim=" << g_aimX << ',' << g_aimY << '\n'
                        << "captureCamera=" << g_cameraYawOffsetDegrees << ','
                        << g_cameraPitchDegrees << '\n';
        }
        SwapBuffers(g_deviceContext);
        return;
    }

    if (g_meshMode && g_sceneVertices != 0) {
        const float aspect = kLogicalWidth / kLogicalHeight;
        std::size_t cameraFrame = 0;
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        if (g_cameraLoaded) {
            applyPerspective(g_titleCamera.fieldOfViewDegrees, aspect, 1.0f, 1000.0f);
            const ULONGLONG elapsed = GetTickCount64() - g_cameraStartTime;
            cameraFrame = static_cast<std::size_t>(
                (elapsed * g_titleCamera.framesPerSecond / 1000ULL) % g_titleCamera.frameCount);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            applyLookAt(g_titleCamera.positions[cameraFrame],
                        g_titleCamera.targets[cameraFrame], g_titleCamera.rollDegrees);
        } else {
            glOrtho(-1.45 * aspect, 1.45 * aspect, -1.45, 1.45, -10.0, 10.0);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            const float centerX = (g_sceneMinimum.x + g_sceneMaximum.x) * 0.5f;
            const float centerY = (g_sceneMinimum.y + g_sceneMaximum.y) * 0.5f;
            const float centerZ = (g_sceneMinimum.z + g_sceneMaximum.z) * 0.5f;
            const float extent = std::max({g_sceneMaximum.x - g_sceneMinimum.x,
                                           g_sceneMaximum.y - g_sceneMinimum.y, 0.001f});
            glRotatef(-18.0f, 1.0f, 0.0f, 0.0f);
            glScalef(2.2f / extent, 2.2f / extent, 2.2f / extent);
            glTranslatef(-centerX, -centerY, -centerZ);
        }
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_TEXTURE_2D);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        for (const auto& part : g_titleScene) {
            glBindTexture(GL_TEXTURE_2D, part.texture);
            glBegin(GL_TRIANGLES);
            for (const std::uint16_t index : part.mesh.indices) {
                const auto& normal = part.mesh.normals[index];
                const auto& uv = part.mesh.texcoords[index];
                const auto& position = part.mesh.positions[index];
                glNormal3f(normal.x, normal.y, normal.z);
                glTexCoord2f(uv.x * part.uvScaleU, uv.y * part.uvScaleV);
                glVertex3f(position.x, position.y, position.z);
            }
            glEnd();
        }
        if (g_gameFlow) {
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, kLogicalWidth, kLogicalHeight, 0.0, -1.0, 1.0);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            if (g_runtimeState == RuntimeState::Title) {
                drawTitleOverlay();
            } else if (g_runtimeState == RuntimeState::MainMenu) {
                drawMainMenuOverlay();
            } else if (g_runtimeState == RuntimeState::Challenge) {
                drawChallengeOverlay();
            } else if (g_runtimeState == RuntimeState::Extra) {
                drawExtraOverlay();
            } else if (g_runtimeState == RuntimeState::Settings) {
                drawSettingsOverlay();
            } else if (g_runtimeState == RuntimeState::StageSelect) {
                drawStageSelectOverlay();
            } else if (g_runtimeState == RuntimeState::Story) {
                drawStoryOverlay();
            } else if (g_runtimeState == RuntimeState::Armory) {
                drawArmoryOverlay();
            } else if (g_runtimeState == RuntimeState::ArmoryConfirm) {
                drawArmoryConfirmOverlay();
            } else {
                if (g_runtimeState == RuntimeState::Playing && g_missionFailed) {
                    drawMissionFailedOverlay();
                } else if (g_runtimeState == RuntimeState::Playing ||
                    g_runtimeState == RuntimeState::Paused) {
                    if (g_mission3dMode && g_missionSceneLoaded) {
                        updateEnemyWaves();
                        updateMissionDamage();
                        updateMissionObjective();
                        drawMissionScene3d();
                        glMatrixMode(GL_PROJECTION);
                        glLoadIdentity();
                        glOrtho(0.0, kLogicalWidth, kLogicalHeight, 0.0, -1.0, 1.0);
                        glMatrixMode(GL_MODELVIEW);
                        glLoadIdentity();
                        drawCombatEffects();
                        drawMissionHud();
                    } else {
                        drawLogicalTexture(g_gameplayReferenceTexture, 0.0F, 0.0F,
                                           kLogicalWidth, kLogicalHeight);
                    }
                    if (g_runtimeState == RuntimeState::Playing) {
                        drawGameplayAimAndShot();
                        drawDamageOverlay();
                    } else {
                        drawPauseOverlay();
                    }
                    if (g_runtimeState == RuntimeState::Playing && g_missionComplete) {
                        drawMissionCompleteOverlay();
                    }
                }
            }
        }
        if (!g_meshFrameLogged) {
            const GLenum error = glGetError();
            std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
            diagnostics << "meshFrameGlError=" << static_cast<unsigned int>(error) << '\n'
                        << "meshFirstCameraFrame=" << cameraFrame << '\n';
            g_meshFrameLogged = true;
        }
        const bool captureReady = !g_captureAttackFrame ||
            (g_runtimeState == RuntimeState::Playing && missionElapsedSeconds() >= 16.8F);
        if (g_captureRequested && !g_captureWritten && captureReady) {
            g_captureWritten = captureViewportBmp(viewport);
            std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
            diagnostics << "captureWritten=" << (g_captureWritten ? 1 : 0) << '\n'
                        << "captureAim=" << g_aimX << ',' << g_aimY << '\n'
                        << "captureCamera=" << g_cameraYawOffsetDegrees << ','
                        << g_cameraPitchDegrees << '\n';
        }
        SwapBuffers(g_deviceContext);
        return;
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, g_texture);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    const float imageAspect = static_cast<float>(g_textureWidth) /
        static_cast<float>(std::max(g_textureHeight, 1));
    const float logicalAspect = kLogicalWidth / kLogicalHeight;
    float width = kLogicalWidth;
    float height = kLogicalHeight;
    if (imageAspect > logicalAspect) {
        height = width / imageAspect;
    } else {
        width = height * imageAspect;
    }
    const float x = (kLogicalWidth - width) * 0.5f;
    const float y = (kLogicalHeight - height) * 0.5f;

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x + width, y);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x + width, y + height);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + height);
    glEnd();

    SwapBuffers(g_deviceContext);
}

bool createOpenGLContext(HWND window) {
    g_deviceContext = GetDC(window);
    PIXELFORMATDESCRIPTOR descriptor{};
    descriptor.nSize = sizeof(descriptor);
    descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    descriptor.iPixelType = PFD_TYPE_RGBA;
    descriptor.cColorBits = 32;
    descriptor.cDepthBits = 24;
    descriptor.iLayerType = PFD_MAIN_PLANE;
    const int format = ChoosePixelFormat(g_deviceContext, &descriptor);
    if (format == 0 || !SetPixelFormat(g_deviceContext, format, &descriptor)) {
        return false;
    }
    g_glContext = wglCreateContext(g_deviceContext);
    if (!g_glContext || !wglMakeCurrent(g_deviceContext, g_glContext)) {
        return false;
    }
    const auto text = [](GLenum name) {
        const auto* value = glGetString(name);
        return value ? reinterpret_cast<const char*>(value) : "unavailable";
    };
    std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::trunc);
    diagnostics << "vendor=" << text(GL_VENDOR) << '\n'
                << "renderer=" << text(GL_RENDERER) << '\n'
                << "version=" << text(GL_VERSION) << '\n'
                << "logicalViewport=480x320\n"
                << "startMode=" << (g_startInGameMode ? "game" :
                    (g_startInFireMode ? "fire" :
                    (g_startInMeshMode ? "mesh" : "texture"))) << '\n';
    return true;
}

void destroyOpenGLContext(HWND window) {
    if (g_glContext) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(g_glContext);
        g_glContext = nullptr;
    }
    if (g_deviceContext) {
        ReleaseDC(window, g_deviceContext);
        g_deviceContext = nullptr;
    }
}

void selectTexture(HWND window, int direction) {
    g_meshMode = false;
    g_fireMode = false;
    const int count = static_cast<int>(kTextures.size());
    g_textureIndex = static_cast<std::size_t>(
        (static_cast<int>(g_textureIndex) + direction + count) % count);
    loadCurrentTexture(window);
    InvalidateRect(window, nullptr, FALSE);
}

void playEngineSound(HWND window) {
    if (!g_settingSound) {
        g_status = L"Sound is disabled in SETTINGS";
        updateWindowTitle(window);
        return;
    }
    const auto path = executableDirectory() / L"assets" / L"engine-start.wav";
    g_status = PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)
        ? L"Recovered Engine_Start WAV playing"
        : L"WAV playback failed";
    updateWindowTitle(window);
}

void beginReload(bool right, ULONGLONG now) {
    int& ammo = right ? g_rightAmmo : g_leftAmmo;
    bool& reloading = right ? g_rightReloading : g_leftReloading;
    ULONGLONG& completeTime = right ?
        g_rightReloadCompleteTime : g_leftReloadCompleteTime;
    if (reloading || ammo > 0) {
        return;
    }
    reloading = true;
    completeTime = now + weaponReloadDuration(g_equippedWeapon);
    std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
    diagnostics << "reloadStart=" << (right ? 'R' : 'L')
                << ",completeAt=" << completeTime << '\n';
}

void updateWeaponReloads() {
    if (g_runtimeState != RuntimeState::Playing) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const int capacity = weaponAmmoCapacity(g_equippedWeapon);
    const auto finishReload = [&](bool right) {
        bool& reloading = right ? g_rightReloading : g_leftReloading;
        ULONGLONG& completeTime = right ?
            g_rightReloadCompleteTime : g_leftReloadCompleteTime;
        int& ammo = right ? g_rightAmmo : g_leftAmmo;
        if (reloading && now >= completeTime) {
            reloading = false;
            completeTime = 0;
            ammo = capacity;
            std::ofstream diagnostics(
                executableDirectory() / "opengl-runtime.txt", std::ios::app);
            diagnostics << "reloadComplete=" << (right ? 'R' : 'L')
                        << ",ammo=" << ammo << '\n';
        }
    };
    finishReload(false);
    finishReload(true);
}

void fireWeapon(HWND window, bool right) {
    if (!g_gameFlow || g_runtimeState != RuntimeState::Playing ||
        g_missionComplete || g_missionFailed) {
        return;
    }
    const WeaponSpec& weapon = kWeaponSpecs[g_equippedWeapon];
    const ULONGLONG now = GetTickCount64();
    int& ammo = right ? g_rightAmmo : g_leftAmmo;
    bool& reloading = right ? g_rightReloading : g_leftReloading;
    ULONGLONG& lastShotTime = right ? g_rightLastShotTime : g_leftLastShotTime;
    if (reloading) {
        return;
    }
    if (ammo <= 0) {
        beginReload(right, now);
        return;
    }
    if (lastShotTime != 0 && now - lastShotTime < weaponCadence(g_equippedWeapon)) {
        return;
    }
    const wchar_t* soundFile = right ? weapon.shotRight : weapon.shotLeft;
    if (soundFile != nullptr && g_settingSound) {
        const auto sound = executableDirectory() / L"assets" / soundFile;
        PlaySoundW(sound.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
    ++g_shotCount;
    ammo = std::max(0, ammo - 1);
    g_shotActive = true;
    g_shotStartTime = now;
    lastShotTime = now;
    int hitEnemy = -1;
    bool primaryDestroyed = false;
    const int upgradedDamage = weaponDamage(g_equippedWeapon);
    if (weapon.behavior == WeaponBehavior::Shield) {
        g_playerShield = std::min(1.0F, g_playerShield + 0.28F +
            static_cast<float>(g_weaponLevels[g_equippedWeapon]) * 0.035F);
        hitEnemy = -2;
    } else if (g_mission3dMode && weapon.behavior != WeaponBehavior::Pulse) {
        const float aimAssistRadius = g_settingMultiTouch
            ? std::max(58.0F, kWeaponTargetSizes[g_equippedWeapon] * 0.72F)
            : std::max(20.0F, kWeaponTargetSizes[g_equippedWeapon] * 0.35F);
        float nearestDistanceSquared = aimAssistRadius * aimAssistRadius;
        for (std::size_t index = 0; index < g_enemies.size(); ++index) {
            if (!g_enemies[index].alive) {
                continue;
            }
            const float dx = g_aimX - g_enemies[index].targetX;
            const float dy = g_aimY - g_enemies[index].targetY;
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared < nearestDistanceSquared) {
                nearestDistanceSquared = distanceSquared;
                hitEnemy = static_cast<int>(index);
            }
        }
    }
    const auto damageEnemy = [](EnemyInstance& enemy, int amount) {
        if (!enemy.alive || amount <= 0) {
            return;
        }
        enemy.hitPoints -= amount;
        if (enemy.hitPoints <= 0) {
            enemy.alive = false;
            enemy.respawnAt = GetTickCount64() + 850ULL;
            g_targetsRemaining = std::max(0, g_targetsRemaining - 1);
        }
    };
    if (weapon.behavior == WeaponBehavior::Pulse) {
        for (auto& enemy : g_enemies) {
            damageEnemy(enemy, upgradedDamage);
        }
        hitEnemy = -3;
    } else if (hitEnemy >= 0) {
        const std::size_t primary = static_cast<std::size_t>(hitEnemy);
        damageEnemy(g_enemies[primary], upgradedDamage);
        primaryDestroyed = !g_enemies[primary].alive;
        if (weapon.behavior == WeaponBehavior::Splash) {
            for (std::size_t index = 0; index < g_enemies.size(); ++index) {
                if (index == primary || !g_enemies[index].alive) {
                    continue;
                }
                const float dx = g_enemies[index].targetX - g_enemies[primary].targetX;
                const float dy = g_enemies[index].targetY - g_enemies[primary].targetY;
                if (dx * dx + dy * dy <= 95.0F * 95.0F) {
                    damageEnemy(g_enemies[index], std::max(1, upgradedDamage / 3));
                }
            }
        } else if (weapon.behavior == WeaponBehavior::Chain) {
            int chained = 0;
            for (std::size_t index = 0; index < g_enemies.size() && chained < 2; ++index) {
                if (index != primary && g_enemies[index].alive) {
                    damageEnemy(g_enemies[index], std::max(1, upgradedDamage / 2));
                    ++chained;
                }
            }
        }
    }
    g_combatEffectStart = GetTickCount64();
    if (weapon.behavior == WeaponBehavior::Shield) {
        g_combatEffect = CombatEffect::Shield;
        g_combatEffectX = kLogicalWidth * 0.5F;
        g_combatEffectY = kLogicalHeight * 0.5F;
    } else if (weapon.behavior == WeaponBehavior::Pulse) {
        g_combatEffect = CombatEffect::Emp;
        g_combatEffectX = kLogicalWidth * 0.5F;
        g_combatEffectY = kLogicalHeight * 0.5F;
    } else if (hitEnemy >= 0) {
        const auto& enemy = g_enemies[static_cast<std::size_t>(hitEnemy)];
        g_combatEffectX = enemy.targetX;
        g_combatEffectY = enemy.targetY;
        g_hitEffectVariant = static_cast<std::size_t>(g_shotCount) %
            g_hitEffectTextures.size();
        if (weapon.behavior == WeaponBehavior::Splash) {
            g_combatEffect = CombatEffect::MissileExplosion;
        } else if (weapon.behavior == WeaponBehavior::Chain) {
            g_combatEffect = CombatEffect::Thunder;
        } else {
            g_combatEffect = primaryDestroyed ?
                CombatEffect::EnemyDie : CombatEffect::Hit;
        }
    } else {
        g_combatEffect = CombatEffect::None;
    }
    if (ammo == 0) {
        beginReload(right, now);
    }
    if (hitEnemy >= 0 && g_targetsRemaining == 0 &&
        selectedMissionSpec().objective != MissionObjective::Survive) {
        finishMission();
    }
    const std::string weaponLabel(weapon.label);
    g_status = L"PLAYING: " +
        std::wstring(weaponLabel.begin(), weaponLabel.end()) + L" shot " +
        std::to_wstring(g_shotCount) +
        L" at (" + std::to_wstring(static_cast<int>(g_aimX)) + L", " +
        std::to_wstring(static_cast<int>(g_aimY)) + L")  " +
        (hitEnemy >= 0 ? L"HIT" : L"MISS") + L"  targets=" +
        std::to_wstring(g_targetsRemaining);
    std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
    diagnostics << "shot=" << g_shotCount << ",aim=" << g_aimX << ',' << g_aimY
                << ",weapon=" << weapon.label
                << ",channel=" << (right ? 'R' : 'L')
                << ",ammo=" << ammo
                << ",hitEnemy=" << hitEnemy
                << ",targetsRemaining=" << g_targetsRemaining << '\n';
    updateWindowTitle(window);
}

void toggleMeshMode(HWND window) {
    g_fireMode = false;
    g_meshMode = !g_meshMode;
    if (g_meshMode) {
        g_textureIndex = 6;
        loadCurrentTexture(window);
        g_status = L"Decoded title scene: " + std::to_wstring(g_sceneVertices) +
            L" vertices / " + std::to_wstring(g_sceneTriangles) +
            L" triangles / 3 meshes / " + std::to_wstring(g_titleCamera.frameCount) +
            L" camera frames  [M: return to texture view]";
    } else {
        g_status = L"2D texture inspection mode  [M: decoded PMOD mesh]";
    }
    updateWindowTitle(window);
}

void toggleFireMode(HWND window) {
    g_meshMode = false;
    g_fireMode = !g_fireMode;
    if (g_fireMode) {
        g_gunFireStartTime = GetTickCount64();
        g_fireObservedFrames = 0;
        g_fireLoopLogged = false;
        g_status = L"Decoded gun_fire PLIV: 11 frames / 10 UV keys / additive blend"
            L"  [F: return to texture view]";
    } else {
        g_status = L"2D texture inspection mode  [M: title scene, F: PLIV gun fire]";
    }
    updateWindowTitle(window);
}

void setRuntimeState(HWND window, RuntimeState state) {
    const RuntimeState previousState = g_runtimeState;
    const ULONGLONG transitionTime = GetTickCount64();
    const bool restartingFailedMission =
        state == RuntimeState::Playing && g_missionFailed;
    if (previousState == RuntimeState::Paused && state == RuntimeState::Playing &&
        g_pauseStartTime != 0 && g_missionStartTime != 0) {
        const ULONGLONG pausedDuration = transitionTime - g_pauseStartTime;
        g_missionStartTime += pausedDuration;
        if (g_leftReloading) g_leftReloadCompleteTime += pausedDuration;
        if (g_rightReloading) g_rightReloadCompleteTime += pausedDuration;
        g_pauseStartTime = 0;
    }
    g_runtimeState = state;
    if (state != RuntimeState::Playing) {
        g_leftTriggerHeld = false;
        g_rightTriggerHeld = false;
    }
    g_gameFlow = true;
    g_meshMode = true;
    g_fireMode = false;
    if (state == RuntimeState::Playing &&
        (previousState == RuntimeState::ArmoryConfirm ||
         restartingFailedMission || g_missionStartTime == 0)) {
        const MissionSpec& mission = selectedMissionSpec();
        for (std::size_t index = 0; index < g_enemies.size(); ++index) {
            auto& enemy = g_enemies[index];
            enemy.alive = index < mission.maxActiveEnemies;
            enemy.respawnAt = 0;
            enemy.hitPoints = mission.enemyHitPoints;
        }
        if (mission.objective == MissionObjective::Boss) {
            g_enemies[0].targetX = kLogicalWidth * 0.5F;
            g_enemies[0].targetY = kLogicalHeight * 0.5F - 8.0F;
        }
        g_shotCount = 0;
        g_leftAmmo = weaponAmmoCapacity(g_equippedWeapon);
        g_rightAmmo = weaponAmmoCapacity(g_equippedWeapon);
        g_leftReloading = false;
        g_rightReloading = false;
        g_leftReloadCompleteTime = 0;
        g_rightReloadCompleteTime = 0;
        g_leftLastShotTime = 0;
        g_rightLastShotTime = 0;
        g_cameraYawOffsetDegrees = 0.0F;
        g_cameraPitchDegrees = 0.0F;
        g_lastCameraUpdateTime = transitionTime;
        g_aimX = kLogicalWidth * 0.5F;
        g_aimY = kLogicalHeight * 0.5F;
        g_targetsRemaining =
            mission.objective == MissionObjective::Survive
                ? 999 : mission.targetCount;
        g_missionComplete = false;
        g_missionFailed = false;
        g_missionStartTime = transitionTime;
        g_pauseStartTime = 0;
        g_lastDamageCycle = -1;
        g_playerShield = 1.0F;
        g_playerHull = 1.0F;
        g_combatEffect = CombatEffect::None;
        g_combatEffectStart = 0;
        g_playerHitEffectStart = 0;
    }
    if (state == RuntimeState::Playing && previousState != RuntimeState::Paused &&
        g_missionStartTime == 0) {
        g_missionStartTime = transitionTime;
    }
    if (state == RuntimeState::Paused && previousState == RuntimeState::Playing) {
        g_pauseStartTime = transitionTime;
    }
    if (state == RuntimeState::Title) {
        playBgm(BgmTrack::Title);
        g_cameraStartTime = GetTickCount64();
        g_status = L"Game flow: TAP SCREEN  [Enter/click: main menu, G: inspector]";
    } else if (state == RuntimeState::MainMenu) {
        g_status = L"Game flow: MAIN MENU  [Enter: campaign, Esc: title]";
    } else if (state == RuntimeState::Challenge) {
        g_status = L"Game flow: CHALLENGE  [SURVIVAL available, Esc: menu]";
    } else if (state == RuntimeState::Extra) {
        g_status = L"Game flow: EXTRA  [Esc: menu]";
    } else if (state == RuntimeState::Settings) {
        g_status = L"Game flow: SETTINGS  [click toggles, Esc: menu]";
    } else if (state == RuntimeState::StageSelect) {
        g_status = L"Game flow: STAGE SELECT  [1-1 through 5-5 available]";
    } else if (state == RuntimeState::Story) {
        g_status = std::wstring(L"Game flow: ") + selectedMissionSpec().title +
            L"  [Enter/click: continue, Esc: stages]";
    } else if (state == RuntimeState::Armory) {
        g_status = L"Game flow: ARMORY  [Enter/click: start, Esc: stages]";
    } else if (state == RuntimeState::ArmoryConfirm) {
        g_status = L"Game flow: CONFIRM LOADOUT  [Enter/click: OK, Esc: armory]";
    } else if (state == RuntimeState::Playing) {
        if (previousState != RuntimeState::Paused) {
            playBgm(selectedMissionBgm());
        }
        g_status = std::wstring(L"Game flow: PLAYING ") + selectedMissionSpec().title +
            L"  [Mouse: aim, LMB/RMB: left/right gun, Esc: pause]";
    } else {
        g_status = L"Game flow: PAUSED  [Esc/Enter: resume]";
    }
    std::ofstream diagnostics(executableDirectory() / "opengl-runtime.txt", std::ios::app);
    diagnostics << "runtimeState=";
    const std::wstring stateText(runtimeStateName());
    diagnostics << std::string(stateText.begin(), stateText.end()) << '\n';
    updateWindowTitle(window);
    InvalidateRect(window, nullptr, FALSE);
}

void toggleGameFlow(HWND window) {
    if (g_gameFlow) {
        stopBgm();
        g_gameFlow = false;
        g_meshMode = false;
        g_runtimeState = RuntimeState::Title;
        g_status = L"2D texture inspection mode  [G: game flow]";
        updateWindowTitle(window);
        return;
    }
    setRuntimeState(window, RuntimeState::Title);
}

void advanceGameFlow(HWND window) {
    if (!g_gameFlow) {
        setRuntimeState(window, RuntimeState::Title);
    } else if (g_runtimeState == RuntimeState::Title) {
        setRuntimeState(window, RuntimeState::MainMenu);
    } else if (g_runtimeState == RuntimeState::MainMenu) {
        if (g_menuSelection == 0U) {
            g_challengeMode = false;
            setRuntimeState(window, RuntimeState::StageSelect);
        } else if (g_menuSelection == 1U) {
            setRuntimeState(window, RuntimeState::Challenge);
        } else if (g_menuSelection == 2U) {
            setRuntimeState(window, RuntimeState::Extra);
        } else if (g_menuSelection == 3U) {
            setRuntimeState(window, RuntimeState::Settings);
        } else {
            constexpr std::array<const wchar_t*, 5> names{{
                L"CAMPAIGN", L"CHALLENGE", L"EXTRA", L"SETTINGS", L"TERMS"}};
            g_status = std::wstring(L"MAIN MENU: ") + names[g_menuSelection] +
                L" selected; submenu reconstruction pending";
            updateWindowTitle(window);
        }
    } else if (g_runtimeState == RuntimeState::Challenge) {
        g_challengeMode = true;
        g_selectedStageTheme = 0U;
        setRuntimeState(window, RuntimeState::Armory);
    } else if (g_runtimeState == RuntimeState::StageSelect) {
        if (g_stageColumn < 5U) {
            g_challengeMode = false;
            g_selectedStageTheme = g_stageColumn;
            setRuntimeState(window, RuntimeState::Story);
        } else {
            g_status = L"STAGE SELECT: campaign missions 1-1 through 5-5 are available";
            updateWindowTitle(window);
        }
    } else if (g_runtimeState == RuntimeState::Story) {
        setRuntimeState(window, RuntimeState::Armory);
    } else if (g_runtimeState == RuntimeState::Armory) {
        setRuntimeState(window, RuntimeState::ArmoryConfirm);
    } else if (g_runtimeState == RuntimeState::ArmoryConfirm) {
        if (kWeaponSpecs[g_equippedWeapon].behavior == WeaponBehavior::Shield &&
            selectedMissionSpec().objective != MissionObjective::Survive) {
            setRuntimeState(window, RuntimeState::Armory);
            g_status = L"ARMORY: SHIELD RECHARGER is support-only; select an offensive weapon";
            updateWindowTitle(window);
            return;
        }
        setRuntimeState(window, RuntimeState::Playing);
    } else if (g_runtimeState == RuntimeState::Paused) {
        setRuntimeState(window, RuntimeState::Playing);
    } else if (g_runtimeState == RuntimeState::Playing && g_missionComplete) {
        setRuntimeState(window, missionHubState());
    } else if (g_runtimeState == RuntimeState::Playing && g_missionFailed) {
        setRuntimeState(window, RuntimeState::Playing);
    }
}

void moveMenuSelection(HWND window, int direction) {
    constexpr int itemCount = 5;
    g_menuSelection = static_cast<std::size_t>(
        (static_cast<int>(g_menuSelection) + direction + itemCount) % itemCount);
    constexpr std::array<const wchar_t*, 5> names{{
        L"CAMPAIGN", L"CHALLENGE", L"EXTRA", L"SETTINGS", L"TERMS"}};
    g_status = std::wstring(L"MAIN MENU: ") + names[g_menuSelection];
    updateWindowTitle(window);
    InvalidateRect(window, nullptr, FALSE);
}

void moveStageSelection(HWND window, int rowDirection, int columnDirection) {
    g_stageRow = static_cast<std::size_t>(
        (static_cast<int>(g_stageRow) + rowDirection + 5) % 5);
    g_stageColumn = static_cast<std::size_t>(
        (static_cast<int>(g_stageColumn) + columnDirection + 7) % 7);
    g_status = L"STAGE SELECT: row=" + std::to_wstring(g_stageRow + 1U) +
        L", column=" + std::to_wstring(g_stageColumn + 1U);
    updateWindowTitle(window);
    InvalidateRect(window, nullptr, FALSE);
}

void updateAimCamera() {
    const ULONGLONG now = GetTickCount64();
    if (g_runtimeState != RuntimeState::Playing) {
        g_lastCameraUpdateTime = now;
        return;
    }
    if (g_lastCameraUpdateTime == 0) {
        g_lastCameraUpdateTime = now;
        return;
    }
    const float deltaSeconds = std::min(
        static_cast<float>(now - g_lastCameraUpdateTime) / 1000.0F, 0.05F);
    g_lastCameraUpdateTime = now;
    constexpr float safeLeft = 135.0F;
    constexpr float safeRight = 345.0F;
    constexpr float safeTop = 82.0F;
    constexpr float safeBottom = 238.0F;
    float horizontal = 0.0F;
    float vertical = 0.0F;
    if (!g_settingTilt) {
        return;
    }
    if (g_aimX < safeLeft) {
        horizontal = -(safeLeft - g_aimX) / safeLeft;
    } else if (g_aimX > safeRight) {
        horizontal = (g_aimX - safeRight) / (kLogicalWidth - safeRight);
    }
    if (g_aimY < safeTop) {
        vertical = (safeTop - g_aimY) / safeTop;
    } else if (g_aimY > safeBottom) {
        vertical = -(g_aimY - safeBottom) / (kLogicalHeight - safeBottom);
    }
    if (g_settingReverseY) vertical = -vertical;
    const float sensitivity = 0.55F +
        static_cast<float>(g_settingSensitivity) * 0.11F;
    g_cameraYawOffsetDegrees = std::clamp(
        g_cameraYawOffsetDegrees + horizontal * 45.0F * sensitivity * deltaSeconds,
        -52.0F, 52.0F);
    g_cameraPitchDegrees = std::clamp(
        g_cameraPitchDegrees + vertical * 34.0F * sensitivity * deltaSeconds,
        -28.0F, 28.0F);
}

void reportMouse(HWND window, LPARAM lParam, const wchar_t* eventName) {
    const RECT viewport = logicalViewport();
    const int x = GET_X_LPARAM(lParam);
    const int y = GET_Y_LPARAM(lParam);
    const int width = std::max(viewport.right - viewport.left, 1L);
    const int height = std::max(viewport.bottom - viewport.top, 1L);
    const int rawLogicalX = static_cast<int>(
        (x - viewport.left) * kLogicalWidth / width);
    const int rawLogicalY = static_cast<int>(
        (y - viewport.top) * kLogicalHeight / height);
    // Menu controls use the entire recovered 480x320 surface. Only live
    // gameplay needs the tighter range that keeps the largest reticle visible.
    const bool clampForReticle = g_gameFlow &&
        g_runtimeState == RuntimeState::Playing &&
        !g_missionComplete && !g_missionFailed;
    const int logicalX = clampForReticle
        ? std::clamp(rawLogicalX, 36, 444)
        : std::clamp(rawLogicalX, 0, static_cast<int>(kLogicalWidth));
    const int logicalY = clampForReticle
        ? std::clamp(rawLogicalY, 36, 284)
        : std::clamp(rawLogicalY, 0, static_cast<int>(kLogicalHeight));
    g_aimX = static_cast<float>(logicalX);
    g_aimY = static_cast<float>(logicalY);
    g_status = std::wstring(eventName) + L" event -> native logical (" +
        std::to_wstring(logicalX) + L", " + std::to_wstring(logicalY) + L")";
    updateWindowTitle(window);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        if (!createOpenGLContext(window)) {
            return -1;
        }
        loadProgress();
        if (g_startWeaponOverride >= 0) {
            g_equippedWeapon = static_cast<std::size_t>(g_startWeaponOverride);
        }
        SetTimer(window, 1, 16, nullptr);
        loadCurrentTexture(window);
        const bool sceneLoaded = loadTitleScene(window);
        loadTitleCamera(window);
        loadMissionScene(window);
        const bool fireLoaded = loadGunFireAnimation(window);
        const bool gameUiLoaded = loadGameUi(window);
        if (sceneLoaded && gameUiLoaded && g_startInGameMode) {
            setRuntimeState(window, g_startRuntimeState);
            if (g_startCompleted && g_startRuntimeState == RuntimeState::Playing) {
                for (auto& enemy : g_enemies) {
                    enemy.alive = false;
                    enemy.respawnAt = 0;
                }
                g_targetsRemaining = 0;
                finishMission();
            }
            if (g_startFailed && g_startRuntimeState == RuntimeState::Playing) {
                g_playerShield = 0.0F;
                g_playerHull = 0.0F;
                g_missionFailed = true;
                stopBgm();
            }
            if (g_startWithShot && g_startRuntimeState == RuntimeState::Playing) {
                fireWeapon(window, false);
                fireWeapon(window, true);
            }
        } else if (sceneLoaded && g_startInMeshMode) {
            toggleMeshMode(window);
        } else if (fireLoaded && g_startInFireMode) {
            toggleFireMode(window);
        }
        return 0;
    }
    case WM_SIZE:
        g_clientWidth = std::max<int>(LOWORD(lParam), 1);
        g_clientHeight = std::max<int>(HIWORD(lParam), 1);
        return 0;
    case WM_TIMER:
        ensureBgmLoop();
        updateAimCamera();
        updateWeaponReloads();
        if (g_leftTriggerHeld) fireWeapon(window, false);
        if (g_rightTriggerHeld) fireWeapon(window, true);
        render();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        render();
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN:
        reportMouse(window, lParam, L"touch");
        if (g_gameFlow && g_runtimeState == RuntimeState::Playing &&
            g_missionFailed) {
            if (g_aimY >= 276.0F && g_aimX <= 155.0F) {
                setRuntimeState(window, missionHubState());
            } else if (g_aimY >= 276.0F && g_aimX >= 325.0F) {
                setRuntimeState(window, RuntimeState::Playing);
            }
            return 0;
        }
        if (g_gameFlow && g_runtimeState == RuntimeState::Playing &&
            !g_missionComplete) {
            g_leftTriggerHeld = true;
            SetCapture(window);
        }
        if (g_gameFlow && (g_runtimeState == RuntimeState::Title ||
                           g_runtimeState == RuntimeState::MainMenu ||
                           g_runtimeState == RuntimeState::Challenge ||
                           g_runtimeState == RuntimeState::Extra ||
                           g_runtimeState == RuntimeState::Settings ||
                           g_runtimeState == RuntimeState::StageSelect ||
                           g_runtimeState == RuntimeState::Story ||
                           g_runtimeState == RuntimeState::Armory ||
                           g_runtimeState == RuntimeState::ArmoryConfirm ||
                           g_runtimeState == RuntimeState::Paused)) {
            if (g_runtimeState == RuntimeState::Challenge) {
                if (g_aimY >= 270.0F && g_aimX <= 135.0F) {
                    setRuntimeState(window, RuntimeState::MainMenu);
                } else if (g_aimX >= 70.0F && g_aimX <= 230.0F &&
                           g_aimY >= 54.0F && g_aimY <= 94.0F) {
                    g_challengeMode = true;
                    g_selectedStageTheme = 0U;
                    setRuntimeState(window, RuntimeState::Armory);
                } else {
                    g_status = L"CHALLENGE: INFINITE missions remain locked";
                    updateWindowTitle(window);
                }
                return 0;
            }
            if (g_runtimeState == RuntimeState::Extra) {
                if (g_aimY >= 270.0F && g_aimX <= 135.0F) {
                    setRuntimeState(window, RuntimeState::MainMenu);
                } else {
                    g_status = L"EXTRA: legacy online destination is unavailable";
                    updateWindowTitle(window);
                }
                return 0;
            }
            if (g_runtimeState == RuntimeState::Settings) {
                if (g_aimY >= 270.0F && g_aimX <= 110.0F) {
                    setRuntimeState(window, RuntimeState::MainMenu);
                    return 0;
                }
                if (g_aimY >= 270.0F && g_aimX >= 275.0F) {
                    resetProgress();
                    g_status = L"SETTINGS: local progress and options reset";
                    updateWindowTitle(window);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                if (g_aimY >= 235.0F && g_aimY < 265.0F &&
                    g_aimX >= 137.0F && g_aimX <= 330.0F) {
                    g_settingSensitivity = std::clamp(
                        static_cast<int>((g_aimX - 137.0F) / 19.0F) + 1, 1, 10);
                } else if (g_aimY >= 57.0F && g_aimY < 237.0F &&
                           g_aimX >= 130.0F && g_aimX <= 350.0F) {
                    const std::size_t row = std::min<std::size_t>(
                        static_cast<std::size_t>((g_aimY - 57.0F) / 30.0F), 5U);
                    const bool left = g_aimX < 242.0F;
                    if (row == 0U) g_settingTilt = left;
                    if (row == 1U) g_settingMultiTouch = left;
                    if (row == 2U) g_settingVibration = left;
                    if (row == 3U) {
                        g_settingSound = left;
                        if (g_settingSound) {
                            playBgm(BgmTrack::Title);
                        } else {
                            stopBgm();
                        }
                    }
                    if (row == 4U) g_settingQuality = left;
                    if (row == 5U) g_settingReverseY = !left;
                }
                saveProgress();
                g_status = L"SETTINGS: local option updated";
                updateWindowTitle(window);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (g_runtimeState == RuntimeState::Armory) {
                if (g_aimY >= 286.0F && g_aimX <= 155.0F) {
                    setRuntimeState(window, missionHubState());
                    return 0;
                }
                if (g_aimY >= 248.0F && g_aimY < 286.0F &&
                    g_aimX >= 145.0F && g_aimX <= 335.0F) {
                    tryUpgradeWeapon(window);
                    return 0;
                }
                if (!(g_aimY >= 286.0F && g_aimX >= 325.0F)) {
                    if (g_aimY >= 38.0F && g_aimY <= 272.0F) {
                        const std::size_t column = std::min<std::size_t>(
                            static_cast<std::size_t>(g_aimX / 160.0F), 2U);
                        const std::size_t row = g_aimY < 111.0F ? 0U :
                            (g_aimY < 190.0F ? 1U : 2U);
                        g_armorySelection = row * 3U + column;
                        const int weaponIndex = kArmorySlotToWeapon[g_armorySelection];
                        if (weaponIndex >= 0) {
                            g_equippedWeapon = static_cast<std::size_t>(weaponIndex);
                            saveProgress();
                            const std::string label(kWeaponSpecs[g_equippedWeapon].label);
                            g_status = L"ARMORY: equipped " +
                                std::wstring(label.begin(), label.end());
                        } else {
                            g_status = L"ARMORY: support platform selected";
                        }
                        updateWindowTitle(window);
                        InvalidateRect(window, nullptr, FALSE);
                        return 0;
                    }
                    g_status = L"ARMORY: select equipment or press START";
                    updateWindowTitle(window);
                    return 0;
                }
            } else if (g_runtimeState == RuntimeState::ArmoryConfirm) {
                if (g_aimY >= 184.0F && g_aimY <= 222.0F &&
                    g_aimX >= 135.0F && g_aimX < 240.0F) {
                    setRuntimeState(window, RuntimeState::Armory);
                    return 0;
                }
                if (!(g_aimY >= 184.0F && g_aimY <= 222.0F &&
                      g_aimX >= 240.0F && g_aimX <= 365.0F)) {
                    return 0;
                }
            } else if (g_runtimeState == RuntimeState::Paused) {
                if (g_aimX >= 130.0F && g_aimX <= 350.0F &&
                    g_aimY >= 158.0F && g_aimY <= 198.0F) {
                    setRuntimeState(window, missionHubState());
                    return 0;
                }
                if (g_aimX >= 130.0F && g_aimX <= 350.0F &&
                    g_aimY >= 115.0F && g_aimY <= 153.0F) {
                    g_cameraYawOffsetDegrees = 0.0F;
                    g_cameraPitchDegrees = 0.0F;
                    g_aimX = kLogicalWidth * 0.5F;
                    g_aimY = kLogicalHeight * 0.5F;
                    g_status = L"PAUSED: camera and reticle centered";
                    updateWindowTitle(window);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                if (!(g_aimX >= 130.0F && g_aimX <= 350.0F &&
                      g_aimY >= 70.0F && g_aimY <= 108.0F)) {
                    return 0;
                }
            }
            if (g_runtimeState == RuntimeState::MainMenu &&
                g_aimX >= 47.0F && g_aimX <= 213.0F &&
                g_aimY >= 76.0F && g_aimY < 236.0F) {
                g_menuSelection = std::min<std::size_t>(
                    static_cast<std::size_t>((g_aimY - 76.0F) / 32.0F), 4U);
            } else if (g_runtimeState == RuntimeState::StageSelect &&
                       g_aimX >= 39.0F && g_aimX < 431.0F &&
                       g_aimY >= 76.0F && g_aimY < 246.0F) {
                g_stageColumn = std::min<std::size_t>(
                    static_cast<std::size_t>((g_aimX - 39.0F) / 56.0F), 6U);
                g_stageRow = std::min<std::size_t>(
                    static_cast<std::size_t>((g_aimY - 76.0F) / 34.0F), 4U);
            } else if (g_runtimeState == RuntimeState::StageSelect &&
                       g_aimX >= 16.0F && g_aimX <= 105.0F &&
                       g_aimY >= 276.0F) {
                setRuntimeState(window, RuntimeState::MainMenu);
                return 0;
            }
            advanceGameFlow(window);
        } else if (g_runtimeState == RuntimeState::Playing && g_missionComplete) {
            setRuntimeState(window, missionHubState());
        } else {
            fireWeapon(window, false);
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (g_gameFlow && g_runtimeState == RuntimeState::Playing &&
            !g_missionComplete && !g_missionFailed) {
            reportMouse(window, lParam, L"right trigger");
            g_rightTriggerHeld = true;
            SetCapture(window);
            fireWeapon(window, true);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_gameFlow && g_runtimeState == RuntimeState::Playing) {
            reportMouse(window, lParam, L"tilt");
        } else if ((wParam & MK_LBUTTON) != 0) {
            reportMouse(window, lParam, L"drag");
        }
        return 0;
    case WM_LBUTTONUP:
        g_leftTriggerHeld = false;
        if (!g_rightTriggerHeld && GetCapture() == window) ReleaseCapture();
        reportMouse(window, lParam, L"release");
        return 0;
    case WM_RBUTTONUP:
        g_rightTriggerHeld = false;
        if (!g_leftTriggerHeld && GetCapture() == window) ReleaseCapture();
        if (g_gameFlow && g_runtimeState == RuntimeState::Playing) {
            reportMouse(window, lParam, L"right release");
        }
        return 0;
    case WM_CONTEXTMENU:
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (!g_gameFlow || g_runtimeState == RuntimeState::Title) {
                DestroyWindow(window);
            } else if (g_runtimeState == RuntimeState::MainMenu) {
                setRuntimeState(window, RuntimeState::Title);
            } else if (g_runtimeState == RuntimeState::Challenge ||
                       g_runtimeState == RuntimeState::Extra ||
                       g_runtimeState == RuntimeState::Settings) {
                setRuntimeState(window, RuntimeState::MainMenu);
            } else if (g_runtimeState == RuntimeState::StageSelect) {
                setRuntimeState(window, RuntimeState::MainMenu);
            } else if (g_runtimeState == RuntimeState::Story) {
                setRuntimeState(window, RuntimeState::StageSelect);
            } else if (g_runtimeState == RuntimeState::Armory) {
                setRuntimeState(window, missionHubState());
            } else if (g_runtimeState == RuntimeState::ArmoryConfirm) {
                setRuntimeState(window, RuntimeState::Armory);
            } else if (g_runtimeState == RuntimeState::Playing) {
                setRuntimeState(window, g_missionFailed ?
                    missionHubState() : RuntimeState::Paused);
            } else {
                setRuntimeState(window, RuntimeState::Playing);
            }
        } else if (g_gameFlow && g_runtimeState == RuntimeState::MainMenu &&
                   (wParam == VK_LEFT || wParam == VK_UP || wParam == VK_PRIOR)) {
            moveMenuSelection(window, -1);
        } else if (g_gameFlow && g_runtimeState == RuntimeState::MainMenu &&
                   (wParam == VK_RIGHT || wParam == VK_DOWN || wParam == VK_NEXT)) {
            moveMenuSelection(window, 1);
        } else if (g_gameFlow && g_runtimeState == RuntimeState::StageSelect &&
                   wParam == VK_LEFT) {
            moveStageSelection(window, 0, -1);
        } else if (g_gameFlow && g_runtimeState == RuntimeState::StageSelect &&
                   wParam == VK_RIGHT) {
            moveStageSelection(window, 0, 1);
        } else if (g_gameFlow && g_runtimeState == RuntimeState::StageSelect &&
                   wParam == VK_UP) {
            moveStageSelection(window, -1, 0);
        } else if (g_gameFlow && g_runtimeState == RuntimeState::StageSelect &&
                   wParam == VK_DOWN) {
            moveStageSelection(window, 1, 0);
        } else if (g_gameFlow && g_runtimeState == RuntimeState::Armory &&
                   wParam == 'U') {
            tryUpgradeWeapon(window);
        } else if (wParam == VK_LEFT || wParam == VK_UP || wParam == VK_PRIOR) {
            selectTexture(window, -1);
        } else if (wParam == VK_RETURN && g_gameFlow) {
            if (g_runtimeState == RuntimeState::Playing && g_missionFailed) {
                setRuntimeState(window, RuntimeState::Playing);
            } else {
                advanceGameFlow(window);
            }
        } else if (wParam == VK_RIGHT || wParam == VK_DOWN || wParam == VK_NEXT || wParam == VK_RETURN) {
            selectTexture(window, 1);
        } else if (wParam == VK_SPACE) {
            if (g_gameFlow && g_runtimeState == RuntimeState::Playing) {
                fireWeapon(window, false);
                fireWeapon(window, true);
            } else {
                playEngineSound(window);
            }
        } else if (wParam == 'M') {
            toggleMeshMode(window);
        } else if (wParam == 'F') {
            toggleFireMode(window);
        } else if (wParam == 'G') {
            toggleGameFlow(window);
        }
        return 0;
    case WM_DESTROY: {
        KillTimer(window, 1);
        stopBgm();
        if (g_texture != 0) {
            glDeleteTextures(1, &g_texture);
            g_texture = 0;
        }
        for (auto& part : g_titleScene) {
            if (part.texture != 0) {
                glDeleteTextures(1, &part.texture);
                part.texture = 0;
            }
        }
        const auto deleteEnvironment = [](auto& scene) {
            for (auto& part : scene) {
                if (part.texture != 0) {
                    glDeleteTextures(1, &part.texture);
                    part.texture = 0;
                }
            }
        };
        deleteEnvironment(g_missionScene);
        deleteEnvironment(g_missionScene2);
        deleteEnvironment(g_missionScene3);
        deleteEnvironment(g_missionScene4);
        deleteEnvironment(g_missionScene5);
        for (auto& part : g_enemySceneParts) {
            if (part.texture != 0) {
                glDeleteTextures(1, &part.texture);
                part.texture = 0;
            }
        }
        for (auto& texture : g_hitEffectTextures) {
            if (texture != 0) glDeleteTextures(1, &texture);
            texture = 0;
        }
        for (auto& texture : g_empFieldTextures) {
            if (texture != 0) glDeleteTextures(1, &texture);
            texture = 0;
        }
        for (GLuint* texture : {&g_enemyDieTexture, &g_playerHitTexture,
             &g_playerShieldHitTexture, &g_missileExplosionTexture,
             &g_shieldFieldTexture, &g_thunderFireTexture, &g_bossLaserTexture}) {
            if (*texture != 0) glDeleteTextures(1, texture);
            *texture = 0;
        }
        for (auto& part : g_weaponSceneParts) {
            if (part.texture != 0) {
                glDeleteTextures(1, &part.texture);
                part.texture = 0;
            }
        }
        if (g_gunFireTexture != 0) {
            glDeleteTextures(1, &g_gunFireTexture);
            g_gunFireTexture = 0;
        }
        for (auto& storyTexture : g_storyTextures) {
            if (storyTexture != 0) {
                glDeleteTextures(1, &storyTexture);
                storyTexture = 0;
            }
        }
        for (auto& targetTexture : g_targetTextures) {
            if (targetTexture != 0) {
                glDeleteTextures(1, &targetTexture);
                targetTexture = 0;
            }
        }
        for (GLuint* texture : {&g_titleUiTexture, &g_menuUiTexture,
                                &g_armoryTexture, &g_armoryConfirmTexture,
                                &g_armoryUi1Texture, &g_armoryUi2Texture,
                                &g_armoryUi3Texture, &g_popupEnTexture,
                                &g_gameoverTexture,
                                &g_gameplayReferenceTexture, &g_pausedReferenceTexture,
                                &g_resultsTexture,
                                &g_cockpitTexture, &g_pauseTexture}) {
            if (*texture != 0) {
                glDeleteTextures(1, texture);
                *texture = 0;
            }
        }
        destroyOpenGLContext(window);
        PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    const std::wstring commandLine = GetCommandLineW();
    g_startInMeshMode = commandLine.find(L"--mesh") != std::wstring::npos;
    g_startInFireMode = commandLine.find(L"--fire") != std::wstring::npos;
    g_startInGameMode = commandLine.find(L"--game") != std::wstring::npos;
    g_mission3dMode = commandLine.find(L"--reference") == std::wstring::npos;
    if (commandLine.find(L"--yaw90") != std::wstring::npos) {
        g_missionYawDegrees = 90.0F;
    } else if (commandLine.find(L"--yaw180") != std::wstring::npos) {
        g_missionYawDegrees = 180.0F;
    } else if (commandLine.find(L"--yaw270") != std::wstring::npos) {
        g_missionYawDegrees = 270.0F;
    }
    if (commandLine.find(L"--survival") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::Playing;
        g_challengeMode = true;
        g_selectedStageTheme = 0U;
    } else if (commandLine.find(L"--menu") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::MainMenu;
    } else if (commandLine.find(L"--challenge") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::Challenge;
    } else if (commandLine.find(L"--extra") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::Extra;
    } else if (commandLine.find(L"--settings") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::Settings;
    } else if (commandLine.find(L"--mission") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::StageSelect;
    } else if (commandLine.find(L"--story") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::Story;
    } else if (commandLine.find(L"--armory") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::Armory;
    } else if (commandLine.find(L"--confirm") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::ArmoryConfirm;
    } else if (commandLine.find(L"--failed") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::Playing;
        g_startFailed = true;
    } else if (commandLine.find(L"--complete") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::Playing;
        g_startCompleted = true;
    } else if (commandLine.find(L"--playing") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::Playing;
    } else if (commandLine.find(L"--paused") != std::wstring::npos) {
        g_startInGameMode = true;
        g_startRuntimeState = RuntimeState::Paused;
    }
    g_captureRequested = commandLine.find(L"--capture") != std::wstring::npos;
    g_captureAttackFrame = commandLine.find(L"--capture-attack") != std::wstring::npos;
    g_startWithShot = commandLine.find(L"--shoot") != std::wstring::npos;
    for (std::size_t index = 0; index < kWeaponSpecs.size(); ++index) {
        const std::wstring flag = L"--weapon" + std::to_wstring(index + 1U);
        if (commandLine.find(flag) != std::wstring::npos) {
            g_equippedWeapon = index;
            g_startWeaponOverride = static_cast<int>(index);
            break;
        }
    }
    for (std::size_t index = 0; index < 5U; ++index) {
        const std::wstring flag = L"--theme" + std::to_wstring(index + 1U);
        if (commandLine.find(flag) != std::wstring::npos) {
            g_selectedStageTheme = index;
            g_stageColumn = index;
            break;
        }
    }
    for (std::size_t index = 0; index < 5U; ++index) {
        const std::wstring flag = L"--level" + std::to_wstring(index + 1U);
        if (commandLine.find(flag) != std::wstring::npos) {
            g_stageRow = index;
            break;
        }
    }
    if (g_startInMeshMode) {
        g_textureIndex = 6;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 1;
    }
    const HRESULT imagingResult = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wicFactory));
    if (FAILED(imagingResult)) {
        CoUninitialize();
        return 2;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        CoUninitialize();
        return 3;
    }

    RECT desired{0, 0, 1000, 600};
    AdjustWindowRectEx(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, desired.right - desired.left, desired.bottom - desired.top,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        CoUninitialize();
        return 4;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_wicFactory.Reset();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
