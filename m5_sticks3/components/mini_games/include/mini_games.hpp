#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mini_games {

constexpr float kFieldWidth = 115.0f;
constexpr float kFieldHeight = 160.0f;

enum class GameId : uint8_t {
    tilt_quest,
    meteor_dodge,
    tap_runner,
};

enum class RunState : uint8_t {
    playing,
    won,
    game_over,
};

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

const char* name(GameId game);
const char* subtitle(GameId game);
bool requires_motion(GameId game);

class TiltQuest {
public:
    static constexpr size_t kWallCount = 4;
    static constexpr float kBallRadius = 5.0f;
    static constexpr float kGoalRadius = 7.0f;
    static constexpr uint32_t kTargetScore = 5;

    void reset(uint32_t seed = 1);
    void update(float elapsed_seconds, float tilt_x, float tilt_y);

    const Point& ball() const { return ball_; }
    const Point& goal() const { return goal_; }
    const std::array<Rect, kWallCount>& walls() const { return walls_; }
    uint32_t score() const { return score_; }
    float seconds_remaining() const { return seconds_remaining_; }
    RunState state() const { return state_; }

private:
    void resolve_wall_collision(const Rect& wall);
    void select_next_goal();

    Point ball_{};
    Point velocity_{};
    Point goal_{};
    std::array<Rect, kWallCount> walls_{};
    uint32_t score_ = 0;
    uint32_t goal_index_ = 0;
    float seconds_remaining_ = 45.0f;
    RunState state_ = RunState::playing;
};

class MeteorDodge {
public:
    struct Meteor {
        Point center{};
        float radius = 4.0f;
        float speed = 60.0f;
    };

    static constexpr size_t kMeteorCount = 5;
    static constexpr float kPlayerY = 145.0f;
    static constexpr float kPlayerRadius = 6.0f;

    void reset(uint32_t seed = 1);
    void update(float elapsed_seconds, float steer);
    bool activate_shield();

    float player_x() const { return player_x_; }
    const std::array<Meteor, kMeteorCount>& meteors() const { return meteors_; }
    uint32_t score() const { return score_; }
    bool shield_active() const { return shield_seconds_ > 0.0f; }
    bool shield_ready() const { return shield_cooldown_seconds_ <= 0.0f; }
    float shield_cooldown_seconds() const { return shield_cooldown_seconds_; }
    RunState state() const { return state_; }

private:
    uint32_t random();
    float random_unit();
    void respawn(Meteor& meteor, float y_ceiling);

    std::array<Meteor, kMeteorCount> meteors_{};
    float player_x_ = kFieldWidth * 0.5f;
    float player_velocity_ = 0.0f;
    float shield_seconds_ = 0.0f;
    float shield_cooldown_seconds_ = 0.0f;
    uint32_t score_ = 0;
    uint32_t random_state_ = 1;
    RunState state_ = RunState::playing;
};

class TapRunner {
public:
    static constexpr float kGroundY = 145.0f;
    static constexpr float kPlayerX = 17.0f;
    static constexpr float kPlayerSize = 11.0f;

    void reset(uint32_t seed = 1);
    void update(float elapsed_seconds);
    bool jump();

    float player_y() const { return player_y_; }
    const Rect& obstacle() const { return obstacle_; }
    uint32_t score() const { return score_; }
    bool on_ground() const { return on_ground_; }
    RunState state() const { return state_; }

private:
    uint32_t random();
    void respawn_obstacle();

    Rect obstacle_{};
    float player_y_ = kGroundY - kPlayerSize;
    float vertical_velocity_ = 0.0f;
    uint32_t score_ = 0;
    uint32_t random_state_ = 1;
    bool on_ground_ = true;
    RunState state_ = RunState::playing;
};

} // namespace mini_games
