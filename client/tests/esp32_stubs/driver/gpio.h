#pragma once

using gpio_num_t = int;
constexpr int GPIO_MODE_OUTPUT = 1;
int gpio_reset_pin(gpio_num_t pin);
int gpio_set_direction(gpio_num_t pin, int mode);
int gpio_set_level(gpio_num_t pin, int level);
