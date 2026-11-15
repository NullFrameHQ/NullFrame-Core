# NullFrame Core

NullFrame Core is a library mod that is meant to contain certain essential core parts of various projects of NullFrame (me) in one place. One of the main reasons is to help other devs, so they can depend on it, use it and save tons of time instead of rebuilding everything. 

---

# What does it currently offer?

As of now, NullFrame Core contains an amusingly fast terrain snapshot-generation engine in C and experimental quad baker. But as I make new projects, I plan to add their core parts to this open-source project too. 

#### Then, what mod's core parts does NullFrame Core contain right now? 

The answer is the core parts of the mod Myelin, specifically the chunk snapshot-generation engine.

---

# Snapshot-generation engine

The mod's terrain generator is built on a native seed-finding library called [Cubiomes](https://github.com/Cubitect/cubiomes). It acts as the engine for the snapshot generator. It's known as a seed-finding tool, but it also has a fast, optimized terrain and biome calculator that produces terrain similar to Vanilla.

## So, is it just Cubiomes baked into a mod?

Of course not. Although NullFrame Core utilizes Cubiomes, it surely offers more than that. 

<details>
<summary>Numbers</summary>

## Numbers

The numbers you are about to see are pure C math, **with almost 0 in-game overheads**, in **.exe** benchmark files. I will include these benchmarks in the GitHub repo, so you can try it in your hardware too. 

<details>
<summary>Hardware setup</summary>

CPU: **Intel I7 13650HX Laptop**\
Lenovo LOQ's **Performance Mode**\
OS: **Windows 11**
</details>

---

## **cubiomes_bench.exe**

Threads used: only **8** Threads \
Downsampling levels: **ALL**\
Metric: **Chunk Snapshots Per Second** 

**Chunk Count Per Run:**

**10000** chunks for 1:1\
**20000** chunks for 1:2\
**4096** chunks for 1:4\
**50000** chunks for 1:8\
**1000000** chunks for 1:16+

| **1:1** | **1:2** | **1:4** | **1:8** | **1:16** | **1:32** | **1:64** | **1:128** |
| ---:| ---:| ---:| ---:| ---:| ---:| ---:| ---:|
| 15,528 | 32,206 | 105,026 | 216,450 | 1,344,086 | 3,571,429  | 6,666,667 | 8,771,930 |

Almost **9 MILLION** chunk snapshots in 1 second on a laptop hardware.

---

## **sweep.exe**

Threads used: **1 - 10** \
Downsample Levels: **ALL** \
Metric: **Chunk Snapshots Per Second** \
Chunk Count Per Run: **Dynamic** 

| Thread Count | **1:1** | **1:2** | **1:4** | **1:8** | **1:16** | **1:32** | **1:64** | **1:128** |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1** | 2,937 | 5,944 | 18,832 | 45,147 | 289,855 | 769,231 | 1,538,462 | 2,222,222 |
| **2** | 3,104 | 7,278 | 21,209 | 55,055 | 364,597 | 967,585 | 1,834,230 | 2,448,727 |
| **3** | 4,453 | 10,465 | 31,515 | 82,331 | 541,785 | 1,449,099 | 2,730,994 | 3,613,366 |
| **4** | 6,050 | 14,184 | 42,078 | 103,041 | 719,020 | 1,917,084 | 3,593,483 | 4,728,128 |
| **5** | 7,318 | 17,346 | 52,306 | 128,802 | 907,214 | 2,418,962 | 4,491,854 | 5,882,353 |
| **6** | 8,267 | 19,998 | 62,767 | 152,388 | 1,065,317 | 2,853,403 | 5,267,195 | 6,896,545 |
| **7** | 9,837 | 23,667 | 73,126 | 176,298 | 1,231,554 | 3,308,516 | 6,116,202 | 7,999,992 |
| **8** | 11,223 | 26,991 | 76,857 | 194,958 | 1,368,047 | 3,668,459 | 6,779,661 | 8,849,558 |
| **9** | 11,674 | 29,508 | 85,375 | 215,546 | 1,514,482 | 4,054,507 | 7,476,628 | 9,779,941 |
| **10** | 12,796 | 31,906 | 92,191 | 232,706 | 1,632,986 | 4,370,625 | 8,048,290 | 10,526,316 |

**2937** Chunk snapshots a second on a single thread and highest detail option. 
And still getting over **2 MILLION** chunk snapshots each second on single thread in lowest detail.

---

## **1,000,000,000 Chunk Test**

Threads used: **10**\
Downsample Levels: **1:16+ Real, 1:8- Estimated**\
Chunk Count Per Run: **1,000,000,000** (1 **Billion**)

+ **World Aligned DownSample Levels 1:16+**

| **Sample Level** | **Chunk Snapshots/sec** | **Seconds** |
|:---:|:---:|:---:|
|       1:16       |   1,548,316    |    645.9    |
|       1:32       |   4,122,696    |    242.6    |
|       1:64       |   7,665,786    |    130.4    |
|      1:128       |   9,980,959    |    100.2    |

**Creating 1 BILLION Chunks in ~100 seconds! While sustaining insane speeds for over 15 minutes.**

+ **Chunk Aligned DownSample Levels** (*Estimated from previous runs*)

| **Sample Level** | **Chunk Snapshots/sec** | **Seconds** | **Minutes** | **Hours** |
| :---: | :---: | :---: | :---: | :---: |
| 1:1 | 12,796 | 78,149 | 1,302 | 21.7 |
| 1:2 | 31,906 | 31,342 | 522 | 8.71 |
| 1:4 | 92,191 | 10,847 | 180.7 | 3.01 |
| 1:8 | 232,706 | 4,297 | 71.6 | 1.19 |

---

</details>

**If we just implemented Cubiomes into a benchmark file, it's hard to believe it would even come close to these numbers on same hardware.**

---

## So, what makes NullFrame Core's chunk snapshot-generation engine so fast?

1. **C, not Java** Terrain generation is done by a relatively faster programming language than Java. No GC pressure, no JVM.
2. **Cubiomes** Cubiomes is mainly known as a seed-finding tool. However, it has highly optimized terrain functions, such as `mapApproxHeight()`, that make it the main engine of NullFrame Core.
3. **SIMD** AVX2 and AVX-512 kernels enable faster speeds.
4. **Multithreading** Just like other mods (e.g C2ME (Concurrent Chunk Management Engine)), this mod also makes a great use of multiple worker threads inside a CPU to achieve parallelism and faster generation speeds.
5. **Terrain "Downsampling"** Being one of few mods to give players an option to choose between Quality & Performance for Minecraft chunk generation. While 1:1 (Native) sampling level is lowest, meaning Cubiomes should sample each block inside a Minecraft chunk, higher downsampling levels like 1:2+ makes Cubiomes take fewer samples inside a chunk and run "Bilinear Interpolation" for connecting the samples to form a full terrain (More performance because engine does less work). And even higher downsampling levels use world-grid aligned sample taking (not limited to samples per chunk) to achieve even higher speeds. Yes, Client (Players) can choose the sampling levels (from 1:1 up to 1:128!) from Client GUI in Myelin mod
6. **Usage of DirectByteBuffer** Instead of pure JNI bridge (a bridge connecting C (engine) <-> Java (Actual Game)), which might have been flooded, NullFrame Core uses JNI with DirectByteBuffer (C writes to RAM, Java reads from RAM) which should make bridge overhead much less.

And a ton of other micro-optimizations.

---

## Are higher downsampling levels worth the speed?

You can test it with my other mod implementing these snapshots to Minecraft incredibly fast with **Myelin** which, by the way, can beat superflat world generation speeds while keeping the terrain very similar to actual Minecraft using NullFrame Core. 

But honestly, doesn't matter - with or without detail correction, I would not consider world-grid aligned downsampling levels (1:16+) to use in-game. With Myelin's advanced caching and other optimizations, speed difference is almost none between downsampling levels in-game while visual difference is not. 

### So, what downsampling levels to consider?

I would recommend 1:1 to 1:8 (1:1, 1:2, 1:4, 1:8). Use 1:1 for maximum detail. But if your main target is more speed but terrain quality still matters, you can go with 1:4 or 1:8. 

Just for reference, 1:4 and 1:2 are actually close to 1:1 in terms of quality. 

#### Difference between Vanilla and NullFrame Core (1:1 downsampling)

**Screenshot is taken using Myelin**

<img src="images/Vanilla%20terrain%20gen%20screenshot.png" alt="Vanilla terrain gen screenshot" width="49.5%" /> <img src="images/NullFrame%20Core%20%2B%20Myelin%20terrain%20gen%20screenshot.png" alt="NullFrame Core + Myelin terrain gen screenshot" width="49.5%" />

Pretty close huh? Although NullFrame Core doesn't generate caves or structures and uses custom vegetation to achieve these insane speeds, the biome and terrain shapes look similar to Vanilla Minecraft.

---

## Compatibility with other mods

By default, NullFrame Core should not create incompatibility issues with other mods because of it being a library mod.

However, don't expect NullFrame Core's snapshot generator to be compatible with other custom world generation mods; it is built on Cubiomes.

---

## Can I use NullFrame Core to build on?

Of course yes! That is literally why NullFrame Core exists. I will try to keep all of the APIs simple to make it easy-to-use. 

You can add NullFrame Core as a dependency and build on top of it. 

Take Myelin for example. It uses NullFrame Core's snapshot generator and implements the snapshots to Minecraft game itself, achieving ridiculous speeds that shouldn't be possible. 

---

## Hardware requirements

+ AVX2-compatible CPU

That's pretty much it. If your computer was built in the last decade, you should be good to go.

---

## Credits

- **[cubiomes](https://github.com/Cubitect/cubiomes)** by Cubitect — the terrain
  generation core.

## License

- LGPL-3.0. Open-source.