/*
 * Copyright (c) 2007, 2009 Joseph Gaeddert
 * Copyright (c) 2007, 2009 Virginia Polytechnic Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// Automatic gain control
//

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "liquid.internal.h"

#define AGC(name)           LIQUID_CONCAT(agc,name)
#define T                   float
#define TC                  float complex

struct AGC(_s) {
    liquid_agc_type type;

    T e_target;     // target signal energy

    // gain variables
    T g;            // current gain value
    T g_min;        // minimum gain value
    T g_max;        // maximum gain value

    // gain control loop filter parameters
    T BT;           // bandwidth-time constant
    T alpha;        // feed-back gain
    T beta;         // feed-forward gain

    // signal energy estimate iir filter state variables
    T e;            // instantaneous estimated signal energy
    T e_hat;        // filtered (average) energy estimate
    T e_prime;      // previous energy estimate

    // is agc locked?
    int is_locked;
};


AGC() AGC(_create)()
{
    AGC() _q = (AGC()) malloc(sizeof(struct AGC(_s)));
    //_q->type = LIQUID_AGC_DEFAULT;
    _q->type = LIQUID_AGC_LOG;
    //_q->type = LIQUID_AGC_EXP;

    // initialize loop filter state variables
    _q->e_prime = 1.0f;
    _q->e_hat = 1.0f;

    // set default gain variables
    _q->g = 1.0f;
    _q->g_min = 1e-6f;
    _q->g_max = 1e+6f;

    AGC(_set_target)(_q, 1.0);
    AGC(_set_bandwidth)(_q, 0.0);

    _q->is_locked = 0;

    return _q;
}

void AGC(_destroy)(AGC() _q)
{
    free(_q);
}

void AGC(_print)(AGC() _q)
{
    printf("agc [rssi: %12.4fdB]:\n", 10*log10(_q->e_target / _q->g));
}

void AGC(_reset)(AGC() _q)
{
    _q->e_prime = 1.0f;
    _q->e_hat = 1.0f;

    AGC(_unlock)(_q);
}

void AGC(_set_target)(AGC() _q, T _e_target)
{
    // validate input; check to ensure _e_target is reasonable
    if (_e_target <= 0.0f) {
        fprintf(stderr,"error: agc_set_target(), target energy must be greater than 0\n");
        exit(-1);
    }

    _q->e_target = _e_target;
}

void AGC(_set_gain_limits)(AGC() _q, T _g_min, T _g_max)
{
    if (_g_min > _g_max) {
        fprintf(stderr,"error: agc_set_gain_limits(), _g_min < _g_max\n");
        exit(-1);
    }

    _q->g_min = _g_min;
    _q->g_max = _g_max;
}

void AGC(_set_bandwidth)(AGC() _q, T _BT)
{
    // check to ensure _BT is reasonable
    if ( _BT < 0 ) {
        fprintf(stderr,"error: agc_set_bandwidth(), bandwidth must be positive\n");
        exit(-1);
    } else if ( _BT > 1.0f ) {
        fprintf(stderr,"error: agc_set_bandwidth(), bandwidth must less than 1.0\n");
        exit(-1);
    }

    _q->BT = _BT;
    _q->alpha = sqrtf(_q->BT);
    _q->beta = 1 - _q->alpha;
}

void AGC(_lock)(AGC() _q)
{
    _q->is_locked = 1;
}

void AGC(_unlock)(AGC() _q)
{
    _q->is_locked = 0;
}

void AGC(_execute)(AGC() _q, TC _x, TC *_y)
{
    if (_q->is_locked) {
        *_y = _x * (_q->g);
        return;
    }

    switch (_q->type) {
    case LIQUID_AGC_DEFAULT:
        AGC(_estimate_gain_default)(_q,_x);
        break;
    case LIQUID_AGC_LOG:
        AGC(_estimate_gain_log)(_q,_x);
        break;
    case LIQUID_AGC_EXP:
        AGC(_estimate_gain_exp)(_q,_x);
        break;
    default:
        // should never get to this condition
        fprintf(stderr,"error: agc_execute(), invalid agc type\n");
        exit(-1);
    }

    // limit gain
    AGC(_limit_gain)(_q);

    // apply gain to input
    *_y = _x * _q->g;
}

T AGC(_get_signal_level)(AGC() _q)
{
    return (_q->e_target / _q->g);
}

T AGC(_get_gain)(AGC() _q)
{
    return _q->g;
}

// 
// internal methods
//

void AGC(_estimate_gain_default)(AGC() _q, TC _x)
{
    float zeta = 0.1f;
    // estimate normalized energy, should be equal to 1.0 when locked
    _q->e = crealf(_x * conj(_x)); // NOTE: crealf used for roundoff error
    _q->e_prime = (_q->e)*zeta + (_q->e_prime)*(1.0f-zeta);
    _q->e_hat = sqrtf(_q->e_prime);// * (_q->g) / (_q->e_target);

    // ideal gain
    T g = _q->e_target / _q->e_hat;

    // accumulated gain
    _q->g = (_q->beta)*(_q->g) + (_q->alpha)*g;
}

void AGC(_estimate_gain_log)(AGC() _q, TC _x)
{
    float zeta = 0.1f;
    // estimate normalized energy, should be equal to 1.0 when locked
    _q->e = crealf(_x * conj(_x)); // NOTE: crealf used for roundoff error
    _q->e_prime = (_q->e)*zeta + (_q->e_prime)*(1.0f-zeta);
    _q->e_hat = sqrtf(_q->e_prime);// * (_q->g) / (_q->e_target);

    // loop filter : compute log
    T gain_ideal = _q->e_target / _q->e_hat;
    T log_gain_error = logf(gain_ideal) - logf(_q->g);

    // adjust gain proportional to log of error
    _q->g *= expf(_q->alpha * log_gain_error);
}

void AGC(_estimate_gain_exp)(AGC() _q, TC _x)
{
    // compute estimate of instantaneous input signal level
    _q->e = crealf(_x * conjf(_x));
    _q->e_hat = sqrtf(_q->e);

    // compute estimate of output signal level
    T e_out = _q->e_hat * _q->g;

    if (e_out > _q->e_target) {
        // decrease gain proportional to energy difference
        _q->g *= 1.0f - _q->beta * (e_out - _q->e_target) / e_out;
    } else {
        // increase gain proportional to energy difference
        _q->g *= 1.0f + _q->beta * (_q->e_target - e_out) / _q->e_target;
    }
}

// limit gain
void AGC(_limit_gain)(AGC() _q)
{
    if ( _q->g > _q->g_max )
        _q->g = _q->g_max;
    else if ( _q->g < _q->g_min )
        _q->g = _q->g_min;
}

