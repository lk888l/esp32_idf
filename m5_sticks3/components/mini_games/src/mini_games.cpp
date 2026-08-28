#include "mini_games.hpp"

#include <algorithm>
#include <cmath>

namespace mini_games {
namespace {

constexpr float kMinimumStep = 0.0f;
constexpr float kMaximumStep = 0.05f;

float clamp_step(float elapsed_seconds)
{
    return std::clamp(elapsed_seconds, kMinimumStep, kMaximumStep);
}

float squared(float value)
{
    return value * value;
}

bool circles_overlap(const Point& first, float first_radius,
                     const Point& second, float second_radius)
{
    const float radius = first_radius + second_radius;
    return squared(first.x - second.x) + squared(first.y - second.y) <= squared(radius);
}

bool rectangles_overlap(const Rect& first, const Rect& second)
{
    return first.x < second.x + second.width && first.x + first.width > second.x &&
           first.y < second.y + second.height && first.y + first.height > second.y;
}

} // namespace

const char* name(GameId game)
{
    switch (game) {
    case GameId::tilt_quest:
        return "TILT QUEST";
    case GameId::meteor_dodge:
        return "METEOR DODGE";
    case GameId::tap_runner:
        return "TAP RUNNER";
    }
    return "ARCADE";
}

const char* subtitle(GameId game)
{
    switch (game) {
    case GameId::tilt_quest:
        return "Tilt through the maze\nand collect 5 beacons";
    case GameId::meteor_dodge:
        return "Lean left or right\nK2 fires a shield";
    case GameId::tap_runner:
        return "Tap K2 to jump\nand chase a high score";
    }
    return "";
}

bool requires_motion(GameId game)
{
    return game != GameId::tap_runner;
}

void TiltQuest::reset(uint32_t seed)
{
    walls_ = {{
        {21.0f, 28.0f, 73.0f, 7.0f},
        {18.0f, 62.0f, 7.0f, 51.0f},
        {45.0f, 91.0f, 61.0f, 7.0f},
        {82.0f, 121.0f, 7.0f, 30.0f},
    }};
    ball_ = {12.0f, 145.0f};
    velocity_ = {};
    score_ = 0;
    goal_index_ = seed % 5U;
    seconds_remaining_ = 45.0f;
    state_ = RunState::playing;
    select_next_goal();
}

void TiltQuest::select_next_goal()
{
    constexpr std::array<Point, 5> goals = {{
        {103.0f, 14.0f},
        {105.0f, 55.0f},
        {36.0f, 77.0f},
        {14.0f, 132.0f},
        {66.0f, 113.0f},
    }};
    goal_ = goals[goal_index_ % goals.size()];
    goal_index_ = (goal_index_ + 2U) % goals.size();
}

void TiltQuest::resolve_wall_collision(const Rect& wall)
{
    const float nearest_x = std::clamp(ball_.x, wall.x, wall.x + wall.width);
    const float nearest_y = std::clamp(ball_.y, wall.y, wall.y + wall.height);
    const float delta_x = ball_.x - nearest_x;
    const float delta_y = ball_.y - nearest_y;
    const float distance_squared = squared(delta_x) + squared(delta_y);
    if (distance_squared >= squared(kBallRadius)) {
        return;
    }

    if (distance_squared > 0.0001f) {
        const float distance = std::sqrt(distance_squared);
        const float normal_x = delta_x / distance;
        const float normal_y = delta_y / distance;
        const float push = kBallRadius - distance;
        ball_.x += normal_x * push;
        ball_.y += normal_y * push;
        const float inward_speed = velocity_.x * normal_x + velocity_.y * normal_y;
        if (inward_speed < 0.0f) {
            velocity_.x -= inward_speed * normal_x;
            velocity_.y -= inward_speed * normal_y;
        }
        return;
    }

    const float left = std::abs(ball_.x - wall.x);
    const float right = std::abs(wall.x + wall.width - ball_.x);
    const float top = std::abs(ball_.y - wall.y);
    const float bottom = std::abs(wall.y + wall.height - ball_.y);
    const float nearest_edge = std::min({left, right, top, bottom});
    if (nearest_edge == left) {
        ball_.x = wall.x - kBallRadius;
        velocity_.x = std::min(velocity_.x, 0.0f);
    } else if (nearest_edge == right) {
        ball_.x = wall.x + wall.width + kBallRadius;
        velocity_.x = std::max(velocity_.x, 0.0f);
    } else if (nearest_edge == top) {
        ball_.y = wall.y - kBallRadius;
        velocity_.y = std::min(velocity_.y, 0.0f);
    } else {
        ball_.y = wall.y + wall.height + kBallRadius;
        velocity_.y = std::max(velocity_.y, 0.0f);
    }
}

void TiltQuest::update(float elapsed_seconds, float tilt_x, float tilt_y)
{
    if (state_ != RunState::playing) {
        return;
    }

    const float step = clamp_step(elapsed_seconds);
    seconds_remaining_ = std::max(0.0f, seconds_remaining_ - step);
    if (seconds_remaining_ <= 0.0f) {
        state_ = RunState::game_over;
        velocity_ = {};
        return;
    }

    constexpr float acceleration = 180.0f;
    constexpr float maximum_speed = 92.0f;
    tilt_x = std::clamp(tilt_x, -1.0f, 1.0f);
    tilt_y = std::clamp(tilt_y, -1.0f, 1.0f);
    velocity_.x += tilt_x * acceleration * step;
    velocity_.y += tilt_y * acceleration * step;
    const float drag = std::exp(-3.2f * step);
    velocity_.x *= drag;
    velocity_.y *= drag;
    const float speed = std::sqrt(squared(velocity_.x) + squared(velocity_.y));
    if (speed > maximum_speed) {
        velocity_.x *= maximum_speed / speed;
        velocity_.y *= maximum_speed / speed;
    }

    ball_.x += velocity_.x * step;
    ball_.y += velocity_.y * step;
    ball_.x = std::clamp(ball_.x, kBallRadius, kFieldWidth - kBallRadius);
    ball_.y = std::clamp(ball_.y, kBallRadius, kFieldHeight - kBallRadius);
    for (const Rect& wall : walls_) {
        resolve_wall_collision(wall);
    }

    if (circles_overlap(ball_, kBallRadius, goal_, kGoalRadius)) {
        ++score_;
        velocity_.x *= 0.35f;
        velocity_.y *= 0.35f;
        if (score_ >= kTargetScore) {
            state_ = RunState::won;
        } else {
            select_next_goal();
        }
    }
}

uint32_t MeteorDodge::random()
{
    random_state_ = random_state_ * 1664525U + 1013904223U;
    return random_state_;
}

float MeteorDodge::random_unit()
{
    return static_cast<float>((random() >> 8U) & 0x00FFFFFFU) / 16777215.0f;
}

void MeteorDodge::respawn(Meteor& meteor, float y_ceiling)
{
    meteor.radius = 3.5f + random_unit() * 3.0f;
    meteor.center.x = meteor.radius +
                      random_unit() * (kFieldWidth - meteor.radius * 2.0f);
    meteor.center.y = y_ceiling - random_unit() * 45.0f;
    meteor.speed = 55.0f + random_unit() * 34.0f + std::min(score_ * 1.8f, 38.0f);
}

void MeteorDodge::reset(uint32_t seed)
{
    random_state_ = seed == 0 ? 1U : seed;
    player_x_ = kFieldWidth * 0.5f;
    player_velocity_ = 0.0f;
    shield_seconds_ = 0.0f;
    shield_cooldown_seconds_ = 0.0f;
    score_ = 0;
    state_ = RunState::playing;
    float y_ceiling = -18.0f;
    for (Meteor& meteor : meteors_) {
        respawn(meteor, y_ceiling);
        y_ceiling -= 31.0f;
    }
}

bool MeteorDodge::activate_shield()
{
    if (state_ != RunState::playing || shield_cooldown_seconds_ > 0.0f) {
        return false;
    }
    shield_seconds_ = 0.72f;
    shield_cooldown_seconds_ = 3.0f;
    return true;
}

void MeteorDodge::update(float elapsed_seconds, float steer)
{
    if (state_ != RunState::playing) {
        return;
    }

    const float step = clamp_step(elapsed_seconds);
    shield_seconds_ = std::max(0.0f, shield_seconds_ - step);
    shield_cooldown_seconds_ = std::max(0.0f, shield_cooldown_seconds_ - step);

    steer = std::clamp(steer, -1.0f, 1.0f);
    player_velocity_ += steer * 420.0f * step;
    player_velocity_ *= std::exp(-7.0f * step);
    player_x_ += player_velocity_ * step;
    player_x_ = std::clamp(player_x_, kPlayerRadius,
                           kFieldWidth - kPlayerRadius);

    const Point player{player_x_, kPlayerY};
    for (Meteor& meteor : meteors_) {
        meteor.center.y += meteor.speed * step;
        if (meteor.center.y - meteor.radius > kFieldHeight) {
            ++score_;
            respawn(meteor, -12.0f);
            continue;
        }
        if (!circles_overlap(player, kPlayerRadius, meteor.center, meteor.radius)) {
            continue;
        }
        if (shield_seconds_ > 0.0f) {
            score_ += 2;
            shield_seconds_ = 0.0f;
            respawn(meteor, -22.0f);
        } else {
            state_ = RunState::game_over;
            player_velocity_ = 0.0f;
            return;
        }
    }
}

uint32_t TapRunner::random()
{
    random_state_ = random_state_ * 1664525U + 1013904223U;
    return random_state_;
}

void TapRunner::respawn_obstacle()
{
    const float variation = static_cast<float>((random() >> 16U) % 18U);
    obstacle_.width = 9.0f + static_cast<float>((random() >> 24U) % 5U);
    obstacle_.height = 16.0f + variation;
    obstacle_.x = kFieldWidth + 24.0f + static_cast<float>((random() >> 20U) % 25U);
    obstacle_.y = kGroundY - obstacle_.height;
}

void TapRunner::reset(uint32_t seed)
{
    random_state_ = seed == 0 ? 1U : seed;
    player_y_ = kGroundY - kPlayerSize;
    vertical_velocity_ = 0.0f;
    score_ = 0;
    on_ground_ = true;
    state_ = RunState::playing;
    respawn_obstacle();
}

bool TapRunner::jump()
{
    if (state_ != RunState::playing || !on_ground_) {
        return false;
    }
    vertical_velocity_ = -214.0f;
    on_ground_ = false;
    return true;
}

void TapRunner::update(float elapsed_seconds)
{
    if (state_ != RunState::playing) {
        return;
    }

    const float step = clamp_step(elapsed_seconds);
    vertical_velocity_ += 510.0f * step;
    player_y_ += vertical_velocity_ * step;
    const float ground_top = kGroundY - kPlayerSize;
    if (player_y_ >= ground_top) {
        player_y_ = ground_top;
        vertical_velocity_ = 0.0f;
        on_ground_ = true;
    }

    const float speed = 76.0f + std::min(score_ * 4.5f, 54.0f);
    obstacle_.x -= speed * step;
    if (obstacle_.x + obstacle_.width < 0.0f) {
        ++score_;
        respawn_obstacle();
    }

    const Rect player{kPlayerX, player_y_, kPlayerSize, kPlayerSize};
    if (rectangles_overlap(player, obstacle_)) {
        state_ = RunState::game_over;
        vertical_velocity_ = 0.0f;
    }
}

} // namespace mini_games
