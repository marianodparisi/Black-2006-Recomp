/* Optional combat tuning at the native health/damage boundaries. The guest
 * still owns armor, damage responses, death callbacks and headshot statistics.
 * These helpers never edit actor memory or the caller's floating-point state. */
static void black_tuning_settings(PcVideoSettings *settings)
{
    fenv_t environment;
    const unsigned csr = _mm_getcsr();
    /* The first read may initialize settings and parse a floating-point FOV. */
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    pc_video_get(settings);
    fesetenv(&environment);
    _mm_setcsr(csr);
}

static int black_tuning_enemy(uint32_t actor, int active)
{
    if (!black_pc_ram_span(actor, 0x3B1u) ||
        GUEST32(actor) != BLACK_VA(0x0028C1E8u) || GUEST32(actor + 0xC4u) != 1u)
        return 0;
    /* Initialization calls the health setter before the controller is always
     * attached. Live bullet hits must belong to an active enemy actor. */
    return !active || (GUEST32(actor + 0x324u) != 0u &&
                       GUEST32(actor + 0x384u) == 0u);
}

static int black_tuning_player_bullet(uint32_t victim, uint32_t shooter)
{
    uint32_t core;
    if (!black_tuning_enemy(victim, 1) ||
        !black_pc_ram_span(BLACK_VA(0x002D45D4u), 4u))
        return 0;
    core = GUEST32(BLACK_VA(0x002D45D4u));
    return black_pc_ram_span(core, 0x34u) && shooter == core + 0x30u;
}

static float black_tuning_scale(float native_value, unsigned percent)
{
    uint32_t bits;
    float result = native_value;
    fenv_t environment;
    const unsigned csr = _mm_getcsr();
    memcpy(&bits, &native_value, sizeof(bits));
    /* Preserve signed zero, negative values and every non-finite bit pattern.
     * At 100% return the original bits without performing arithmetic. */
    if (percent == 100u || bits == 0u || bits >= 0x7F800000u)
        return native_value;
    fegetenv(&environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    result = native_value * ((float)percent / 100.0f);
    memcpy(&bits, &result, sizeof(bits));
    if (bits >= 0x7F800000u)
        result = native_value;
    fesetenv(&environment);
    _mm_setcsr(csr);
    return result;
}

float black_tuning_spawn_health(uint32_t actor, float native_health)
{
    PcVideoSettings settings;
    unsigned percent;
    black_tuning_settings(&settings);
    percent = settings.enemy_health;
    if ((percent != 25u && percent != 50u && percent != 75u && percent != 100u &&
         percent != 150u && percent != 200u && percent != 300u && percent != 400u) ||
        !black_tuning_enemy(actor, 0))
        return native_health;
    return black_tuning_scale(native_health, percent);
}

float black_tuning_bullet_damage(uint32_t victim, uint32_t shooter, float native_damage)
{
    PcVideoSettings settings;
    unsigned percent;
    black_tuning_settings(&settings);
    percent = settings.bullet_damage;
    if ((percent != 50u && percent != 100u && percent != 150u && percent != 200u &&
         percent != 300u && percent != 400u && percent != 600u && percent != 1000u) ||
        !black_tuning_player_bullet(victim, shooter))
        return native_damage;
    return black_tuning_scale(native_damage, percent);
}

unsigned black_tuning_headshot(uint32_t victim, uint32_t shooter,
                              unsigned hit_part, unsigned native_kill)
{
    PcVideoSettings settings;
    black_tuning_settings(&settings);
    /* Native hit-part zero is the head. Returning the native kill flag leaves
     * the complete guest death path in charge, including its headshot counter. */
    if (settings.headshot_instakill == 1u && hit_part == 0u &&
        black_tuning_player_bullet(victim, shooter) && GUEST8(victim + 0x3B0u) == 0u)
        return 1u;
    return native_kill;
}
