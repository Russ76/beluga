// Copyright 2022-2023 Ekumen, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BELUGA_MOTION_DIFFERENTIAL_DRIVE_MODEL_HPP
#define BELUGA_MOTION_DIFFERENTIAL_DRIVE_MODEL_HPP

#include <optional>
#include <random>
#include <shared_mutex>

#include <sophus/se2.hpp>
#include <sophus/so2.hpp>

/**
 * \file
 * \brief Implementation of a differential drive odometry motion model.
 */

namespace beluga {

/// Parameters to construct a DifferentialDriveModel instance.
/**
 * See Probabilistic Robotics \cite thrun2005probabilistic Chapter 5.4.2, particularly table 5.6.
 */
struct DifferentialDriveModelParam {
  /// Rotational noise from rotation
  /**
   * How much rotational noise is generated by the relative rotation between the last two odometry updates.
   * Also known as `alpha1`.
   */
  double rotation_noise_from_rotation;
  /// Rotational noise from translation
  /**
   * How much rotational noise is generated by the relative translation between the last two odometry updates.
   * Also known as `alpha2`.
   */
  double rotation_noise_from_translation;
  /// Translational noise from translation
  /**
   * How much translational noise is generated by the relative translation between the last two odometry updates.
   * Also known as `alpha3`.
   */
  double translation_noise_from_translation;
  /// Translational noise from rotation
  /**
   * How much translational noise is generated by the relative rotation between the last two odometry updates.
   * Also known as `alpha4`.
   */
  double translation_noise_from_rotation;

  /// Distance threshold to detect in-place rotation.
  double distance_threshold = 0.01;
};

/// Sampled odometry model for a differential drive.
/**
 * This class implements OdometryMotionModelInterface2d and satisfies \ref MotionModelPage.
 *
 * See Probabilistic Robotics \cite thrun2005probabilistic Chapter 5.4.2.
 *
 * \tparam Mixin The mixed-in type with no particular requirements.
 */
template <class Mixin>
class DifferentialDriveModel : public Mixin {
 public:
  /// Update type of the motion model, same as the state_type in the odometry model.
  using update_type = Sophus::SE2d;
  /// State type of a particle.
  using state_type = Sophus::SE2d;

  /// Parameter type that the constructor uses to configure the motion model.
  using param_type = DifferentialDriveModelParam;

  /// Constructs a DifferentialDriveModel instance.
  /**
   * \tparam ...Args Arguments types for the remaining mixin constructors.
   * \param params Parameters to configure this instance.
   *  See beluga::DifferentialDriveModelParam for details.
   * \param ...args Arguments that are not used by this part of the mixin, but by others.
   */
  template <class... Args>
  explicit DifferentialDriveModel(const param_type& params, Args&&... args)
      : Mixin(std::forward<Args>(args)...), params_{params} {}

  /// Applies the last motion update to the given particle state.
  /**
   * \tparam Generator  A random number generator that must satisfy the
   *  [UniformRandomBitGenerator](https://en.cppreference.com/w/cpp/named_req/UniformRandomBitGenerator)
   *  requirements.
   * \param state The state of the particle to which the motion will be applied.
   * \param gen An uniform random bit generator object.
   */
  template <class Generator>
  [[nodiscard]] state_type apply_motion(const state_type& state, Generator& gen) const {
    static thread_local auto distribution = std::normal_distribution<double>{};
    const auto lock = std::shared_lock<std::shared_mutex>{params_mutex_};
    const auto first_rotation = Sophus::SO2d{distribution(gen, first_rotation_params_)};
    const auto translation = Eigen::Vector2d{distribution(gen, translation_params_), 0.0};
    const auto second_rotation = Sophus::SO2d{distribution(gen, second_rotation_params_)};
    return state * Sophus::SE2d{first_rotation, Eigen::Vector2d{0.0, 0.0}} * Sophus::SE2d{second_rotation, translation};
  }

  /// \copydoc OdometryMotionModelInterface2d::update_motion(const Sophus::SE2d&)
  void update_motion(const update_type& pose) final {
    if (last_pose_) {
      const auto translation = pose.translation() - last_pose_.value().translation();
      const double distance = translation.norm();
      const double distance_variance = distance * distance;

      const auto& previous_orientation = last_pose_.value().so2();
      const auto& current_orientation = pose.so2();
      const auto first_rotation =
          distance > params_.distance_threshold
              ? Sophus::SO2d{std::atan2(translation.y(), translation.x())} * previous_orientation.inverse()
              : Sophus::SO2d{0.0};
      const auto second_rotation = current_orientation * previous_orientation.inverse() * first_rotation.inverse();
      const auto combined_rotation = first_rotation * second_rotation;

      {
        const auto lock = std::lock_guard<std::shared_mutex>{params_mutex_};
        first_rotation_params_ = DistributionParam{
            first_rotation.log(), std::sqrt(
                                      params_.rotation_noise_from_rotation * rotation_variance(first_rotation) +
                                      params_.rotation_noise_from_translation * distance_variance)};
        translation_params_ = DistributionParam{
            distance, std::sqrt(
                          params_.translation_noise_from_translation * distance_variance +
                          params_.translation_noise_from_rotation * rotation_variance(combined_rotation))};
        second_rotation_params_ = DistributionParam{
            second_rotation.log(), std::sqrt(
                                       params_.rotation_noise_from_rotation * rotation_variance(second_rotation) +
                                       params_.rotation_noise_from_translation * distance_variance)};
      }
    }
    last_pose_ = pose;
  }

  /// Recovers the latest motion update.
  /**
   * \return Last motion update received by the model or an empty optional if no update was received.
   */
  [[nodiscard]] std::optional<update_type> latest_motion_update() const { return last_pose_; }

 private:
  using DistributionParam = typename std::normal_distribution<double>::param_type;

  DifferentialDriveModelParam params_;
  std::optional<update_type> last_pose_;

  DistributionParam first_rotation_params_{0.0, 0.0};
  DistributionParam second_rotation_params_{0.0, 0.0};
  DistributionParam translation_params_{0.0, 0.0};
  mutable std::shared_mutex params_mutex_;

  static double rotation_variance(const Sophus::SO2d& rotation) {
    // Treat backward and forward motion symmetrically for the noise models.
    const auto flipped_rotation = rotation * Sophus::SO2d{Sophus::Constants<double>::pi()};
    const auto delta = std::min(std::abs(rotation.log()), std::abs(flipped_rotation.log()));
    return delta * delta;
  }
};

}  // namespace beluga

#endif
