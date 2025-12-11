#pragma once

#include <string_view>

#include "aeronet/static-string-view-helpers.hpp"

namespace aeronet {

inline static constexpr std::string_view kAeronetSvg =
    R"(<svg width="80" height="44" viewBox="0 0 80 44" fill="none" xmlns="http://www.w3.org/2000/svg">
  <!-- Aerodynamic A-shape -->
  <path d="M8 36 L22 6 L32 6 L46 36 L38 36 L27 14 L16 36 Z" fill="#2196f3"/>
  <!-- Orange flight trail -->
  <path d="M48 20 C58 20 66 24 72 32 L66 36 C62 30 56 26 48 26 Z" fill="#ff9800"/>
</svg>)";

namespace internal {

inline static constexpr std::string_view k404NotFoundTemplate11 =
    R"(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><title>404 • aeronet</title>
<style>
:root{color-scheme:light;}
body{margin:0;font-family:"Inter",-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:linear-gradient(135deg,#eef2ff,#fdf2ff);color:#1f2540;display:flex;min-height:100vh;align-items:center;justify-content:center;}
.card{background:#fff;border-radius:28px;box-shadow:0 25px 70px rgba(53,38,90,.15);padding:3rem 3.5rem;text-align:center;max-width:480px;}
.logo{margin-bottom:1.5rem;}
h1{font-size:3rem;margin:.2rem 0;}
p{margin:.25rem 0 1rem;font-size:1.05rem;color:#5f5b76;}
a{display:inline-flex;gap:.4rem;align-items:center;padding:.85rem 1.6rem;border-radius:999px;background:#5a4bff;color:#fff;text-decoration:none;font-weight:600;}
.small{font-size:.85rem;color:#8b86a8;}
</style></head><body>
<div class="card">)";

inline static constexpr std::string_view k404NotFoundTemplate12 =
    R"(<div class="small">aeronet routing</div>
<h1>404</h1>
<p>The waypoint you dialed isn’t broadcasting.</p>
<a href="/">Return to base</a>
</div>
</body></html>)";

inline static constexpr std::string_view k404NotFoundTemplate21 =
    R"(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><title>aeronet • 404</title>
<style>
body{margin:0;font-family:"Space Grotesk","Segoe UI",sans-serif;background:#05040b;color:#d9dcff;display:grid;place-items:center;min-height:100vh;}
.panel{padding:3.5rem 3rem;border-radius:32px;background:radial-gradient(circle at top,#0f1235 0%,#05040b 70%);box-shadow:0 25px 60px rgba(11,7,24,.65);text-align:center;max-width:420px;}
.logo{margin-bottom:2rem;}
h1{font-size:3.2rem;letter-spacing:.08em;margin:.1em 0;}
p{margin:.4rem 0 1.6rem;color:#9aa1ff;}
a{color:#fff;text-decoration:none;border:1px solid rgba(255,255,255,.3);border-radius:999px;padding:.85rem 1.9rem;display:inline-flex;align-items:center;gap:.5rem;background:rgba(124,118,255,.2);backdrop-filter:blur(6px);}
a:hover{border-color:#fff;}
small{display:block;margin-top:1.4rem;color:#5d63b8;letter-spacing:.3em;}
</style></head><body>
<section class="panel">)";

inline static constexpr std::string_view k404NotFoundTemplate22 = R"(<h1>NOT FOUND</h1>
<p>The route table has no entry for this beacon.</p>
<a href="/">Navigate home →</a>
<small>404 • aeronet</small>
</section>
</body></html>)";

inline static constexpr std::string_view k404NotFoundTemplate31 =
    R"(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><title>404</title>
<style>
body{margin:0;font-family:"Source Sans 3","Segoe UI",sans-serif;background:#f9fafb;color:#1b1d33;display:flex;align-items:center;justify-content:center;min-height:100vh;padding:1.5rem;}
.card{max-width:460px;background:#fff;border-radius:22px;border:1px solid #e2e6f0;padding:2.5rem;box-shadow:0 35px 80px rgba(12,25,68,.07);}
.logo{margin-bottom:1.25rem;}
h1{font-family:"Playfair Display",serif;font-size:3rem;margin:.1rem 0;}
p{margin:.6rem 0 1.5rem;font-size:1.05rem;color:#4b4f66;}
a{display:inline-block;padding:.85rem 1.6rem;border-radius:12px;background:#1b1d33;color:#fff;text-decoration:none;font-weight:600;}
small{display:block;margin-top:1.3rem;color:#8e94b5;letter-spacing:.18em;}
</style></head><body>
<article class="card">)";

inline static constexpr std::string_view k404NotFoundTemplate32 = R"(<h1>Page Missing</h1>
<p>Looks like this frequency is silent. Try the hangar instead.</p>
<a href="/">Head to /home</a>
<small>aeronet · 404</small>
</article>
</body></html>)";

}  // namespace internal

inline static constexpr std::string_view k404NotFoundTemplate1 =
    JoinStringView_v<internal::k404NotFoundTemplate11, kAeronetSvg, internal::k404NotFoundTemplate12>;

inline static constexpr std::string_view k404NotFoundTemplate2 =
    JoinStringView_v<internal::k404NotFoundTemplate21, kAeronetSvg, internal::k404NotFoundTemplate22>;

inline static constexpr std::string_view k404NotFoundTemplate3 =
    JoinStringView_v<internal::k404NotFoundTemplate31, kAeronetSvg, internal::k404NotFoundTemplate32>;

}  // namespace aeronet