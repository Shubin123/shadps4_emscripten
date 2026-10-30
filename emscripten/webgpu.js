// WebGPU side of the browser build's renderer (video_core/renderer_web/web_gpu.cpp).
//
// The GPU device lives on the page's main thread: WebGPU's promises (device creation, error
// scopes, buffer readback) only settle on a thread whose event loop runs, and the emulator's own
// threads are pthreads (workers) busy in C++. They record work as batches - JSON commands plus an
// upload area of data copied out of guest memory - and post them here with
// MAIN_THREAD_ASYNC_EM_ASM; batches run strictly one after another, in the order they were
// posted. A thread that needs results (readbacks) blocks on a futex word in linear memory until
// the batch has run and its results are in memory.
//
// Before the runtime starts, a device is requested unless the page set Module.shadps4Gpu itself
// (to share its device with presentation) or set Module.shadps4GpuMode = "off". Without one, the
// rasterizer drops GPU work as it did before this backend existed. Under Node, a WebGPU
// implementation can be provided as globalThis.navigator.gpu (scripts/run_node.sh does that with
// Dawn's node bindings).
//
// Commands (ids are assigned on the C++ side and never reused):
//   shader {id, code}                     a WGSL module
//   pipeline {id, kind: render|compute, ...} a pipeline; see createPipeline
//   texture {id, format, size, dimension, levels, renderTarget, viewFormats}
//   destroyTexture {texture}
//   write {texture, level, offset, bytesPerRow, rowsPerImage, size}   upload-area data -> texture
//   clearTexture {texture, color | depth, stencil}
//   copyTexture {src, dst, size}
//   draw {pipeline, pass, groups, scissor, blendConstant, stencilReference, vertex/index counts}
//   dispatch {pipeline, groups, count}
//   readBuffer {offset, size, ptr}         upload-area range (as left by the GPU) -> memory
//   readTexture {texture, width, height, bytesPerRow, ptr}
// Bind group entries: {b, buf: [offset, size]} (in the upload area), {b, tex, view, copy,
// depthAsColor}, {b, dummy, dimension}, {b, samp: GPUSamplerDescriptor}.

if (!ENVIRONMENT_IS_PTHREAD) {
  Module["preRun"] = [].concat(Module["preRun"] || [], function () {
    if (Module["shadps4Gpu"] || Module["shadps4GpuMode"] === "off" ||
        typeof navigator === "undefined" || !navigator.gpu) {
      return;
    }
    addRunDependency("shadps4-webgpu");
    shadps4GpuCreateDevice().then(function (gpu) {
      Module["shadps4Gpu"] = gpu;
    }, function (e) {
      console.warn("[webgpu] no device: " + e);
    }).finally(function () {
      removeRunDependency("shadps4-webgpu");
    });
  });
}

async function shadps4GpuCreateDevice() {
  var adapter = await navigator.gpu.requestAdapter({powerPreference: "high-performance"});
  if (!adapter) {
    throw new Error("no adapter");
  }
  // GCN workgroups go up to 1024 threads and 64KB of LDS, and guest buffers can be large: ask
  // for everything the adapter has rather than WebGPU's conservative defaults.
  var wanted = ["maxComputeInvocationsPerWorkgroup", "maxComputeWorkgroupSizeX",
                "maxComputeWorkgroupSizeY", "maxComputeWorkgroupSizeZ",
                "maxComputeWorkgroupStorageSize", "maxComputeWorkgroupsPerDimension",
                "maxStorageBufferBindingSize", "maxBufferSize", "maxStorageBuffersPerShaderStage",
                "maxStorageTexturesPerShaderStage", "maxSampledTexturesPerShaderStage",
                "maxSamplersPerShaderStage", "maxBindingsPerBindGroup", "maxTextureDimension2D",
                "maxTextureArrayLayers", "maxInterStageShaderVariables", "maxColorAttachments",
                "maxColorAttachmentBytesPerSample", "maxUniformBufferBindingSize"];
  var limits = {};
  for (var name of wanted) {
    if (adapter.limits[name] !== undefined) {
      limits[name] = adapter.limits[name];
    }
  }
  var optional = ["subgroups", "texture-compression-bc", "float32-filterable",
                  "depth-clip-control", "depth32float-stencil8", "dual-source-blending",
                  "rg11b10ufloat-renderable", "texture-formats-tier1", "texture-formats-tier2"];
  var features = optional.filter(function (f) { return adapter.features.has(f); });
  var device = await adapter.requestDevice({requiredFeatures: features, requiredLimits: limits});
  device.lost.then(function (info) {
    console.error("[webgpu] device lost: " + info.message);
  });
  device.addEventListener("uncapturederror", function (e) {
    shadps4GpuLogError("uncaptured: " + e.error.message);
  });
  var ai = adapter.info || {};
  return {device: device, shaders: new Map(), pipelines: new Map(), textures: new Map(),
          samplers: new Map(), dummies: new Map(), chain: Promise.resolve(), errors: 0,
          description: [ai.vendor, ai.architecture, ai.description].filter(Boolean).join(" ")};
}

function shadps4GpuLogError(message) {
  var gpu = Module["shadps4Gpu"];
  // A broken pipeline or binding repeats every frame; the first ones say what's wrong.
  if (!gpu || gpu.errors++ < 32) {
    console.error("[webgpu] " + message);
  }
}

// A module that doesn't compile breaks every pipeline using it: say where, with the WGSL around
// the error (the emulator's log has which guest shader it came from).
function shadps4GpuReportCompilation(module, id, code) {
  module.getCompilationInfo().then(function (info) {
    var lines = code.split("\n");
    for (var m of info.messages) {
      if (m.type !== "error") {
        continue;
      }
      var context = lines.slice(Math.max(0, m.lineNum - 3), m.lineNum + 1).join("\n");
      shadps4GpuLogError("shader " + id + " line " + m.lineNum + ": " + m.message + "\n" +
                         lines[0] + "\n...\n" + context);
    }
  });
}

// Heap views go stale when another thread grows memory; Emscripten's own code checks this on
// every access, code in a --pre-js has to do it itself.
function shadps4GpuHeap() {
  if (wasmMemory.buffer != HEAPU8.buffer) {
    updateMemoryViews();
  }
  return HEAPU8;
}

function shadps4GpuU64(u32s, i) {
  return u32s[i] + u32s[i + 1] * 4294967296;
}

function shadps4GpuViewDimensionFromType(type) {
  if (type.indexOf("cube_array") >= 0) return "cube-array";
  if (type.indexOf("cube") >= 0) return "cube";
  if (type.indexOf("2d_array") >= 0) return "2d-array";
  if (type.indexOf("3d") >= 0) return "3d";
  if (type.indexOf("1d") >= 0) return "1d";
  return "2d";
}

function shadps4GpuLayoutEntry(desc, visibility) {
  var entry = {binding: desc.b, visibility: visibility};
  var type = desc.type || "";
  switch (desc.kind) {
  case "uniform":
    entry.buffer = {type: "uniform"};
    break;
  case "buffer":
    entry.buffer = {type: desc.writable ? "storage" : "read-only-storage"};
    break;
  case "sampler":
    entry.sampler = {type: desc.comparison ? "comparison" : "filtering"};
    break;
  case "storageTexture": {
    // texture_storage_2d<rgba8unorm, write>
    var args = type.slice(type.indexOf("<") + 1, type.lastIndexOf(">")).split(",");
    var access = args[1].trim();
    entry.storageTexture = {format: args[0].trim(),
                            access: access === "read_write" ? "read-write"
                                  : access === "read" ? "read-only" : "write-only",
                            viewDimension: shadps4GpuViewDimensionFromType(type)};
    break;
  }
  case "texture": {
    var sampleType = type.startsWith("texture_depth") ? "depth"
                   : type.indexOf("<u32") >= 0 ? "uint"
                   : type.indexOf("<i32") >= 0 ? "sint" : "float";
    var ms = type.indexOf("multisampled") >= 0;
    if (ms && sampleType === "float") {
      sampleType = "unfilterable-float";
    }
    entry.texture = {sampleType: sampleType, viewDimension: shadps4GpuViewDimensionFromType(type),
                     multisampled: ms};
    break;
  }
  }
  return entry;
}

function shadps4GpuCreatePipeline(gpu, desc) {
  var device = gpu.device;
  var stages = desc.kind === "compute" ? [GPUShaderStage.COMPUTE]
             : [GPUShaderStage.VERTEX, GPUShaderStage.FRAGMENT, GPUShaderStage.VERTEX];
  var layouts = desc.layouts.map(function (entries, i) {
    return device.createBindGroupLayout({entries: entries.map(function (e) {
      return shadps4GpuLayoutEntry(e, stages[i]);
    })});
  });
  var layout = device.createPipelineLayout({bindGroupLayouts: layouts});
  if (desc.kind === "compute") {
    return device.createComputePipelineAsync({
      layout: layout,
      compute: {module: gpu.shaders.get(desc.module), entryPoint: "main"},
    }).then(function (p) { return {pipeline: p, layouts: layouts, descs: desc.layouts}; });
  }
  var render = {
    layout: layout,
    vertex: {module: gpu.shaders.get(desc.vertex), entryPoint: "main"},
    primitive: desc.primitive,
    multisample: {count: 1},
  };
  if (desc.fragment) {
    render.fragment = {module: gpu.shaders.get(desc.fragment), entryPoint: "main",
                       targets: desc.targets.map(function (t) { return t || null; })};
  }
  if (desc.depthStencil) {
    render.depthStencil = desc.depthStencil;
  }
  return device.createRenderPipelineAsync(render).then(function (p) {
    return {pipeline: p, layouts: layouts, descs: desc.layouts};
  });
}

function shadps4GpuTexture(gpu, id) {
  var t = gpu.textures.get(id);
  if (!t) {
    throw new Error("texture " + id + " doesn't exist");
  }
  return t;
}

function shadps4GpuDummy(gpu, kind, dimension, multisampled) {
  var key = kind + ":" + dimension + ":" + multisampled;
  var view = gpu.dummies.get(key);
  if (view) {
    return view;
  }
  var format = kind === "u" ? "rgba8uint" : kind === "s" ? "rgba8sint"
             : kind === "d" ? "depth32float" : "rgba8unorm";
  var layers = dimension.startsWith("cube") ? 6 : 1;
  var texture = gpu.device.createTexture({
    size: [1, 1, layers], format: format, sampleCount: multisampled ? 4 : 1,
    dimension: dimension === "3d" ? "3d" : "2d",
    usage: GPUTextureUsage.TEXTURE_BINDING |
           (multisampled || kind === "d" ? GPUTextureUsage.RENDER_ATTACHMENT : 0),
  });
  view = texture.createView({dimension: dimension});
  gpu.dummies.set(key, view);
  return view;
}

// Runs one batch (layout: struct JsBatch in web_gpu.cpp) and, if someone waits, signals its done
// word: 1 = finished with readbacks in memory, 2 = failed (logged here).
Module["shadps4GpuSubmit"] = function (block) {
  var gpu = Module["shadps4Gpu"];
  gpu.chain = gpu.chain.then(function () {
    return shadps4GpuRunBatch(gpu, block);
  });
};

async function shadps4GpuRunBatch(gpu, block) {
  var heap = shadps4GpuHeap();
  var u32s = new Uint32Array(heap.buffer, block, 10);
  var jsonPtr = shadps4GpuU64(u32s, 0), jsonSize = shadps4GpuU64(u32s, 2);
  var uploadPtr = shadps4GpuU64(u32s, 4), uploadSize = shadps4GpuU64(u32s, 6);
  var done = shadps4GpuU64(u32s, 8);
  var status = 1;
  var transient = [];
  try {
    var commands = JSON.parse(new TextDecoder().decode(heap.slice(jsonPtr, jsonPtr + jsonSize)));
    var upload = heap.slice(uploadPtr, uploadPtr + uploadSize);
    await shadps4GpuRun(gpu, commands, upload, transient);
  } catch (e) {
    shadps4GpuLogError(e && e.stack ? e.stack : String(e));
    status = 2;
  }
  for (var object of transient) {
    object.destroy();
  }
  _shadps4_gpu_release(BigInt(block));
  if (done) {
    var words = new Int32Array(shadps4GpuHeap().buffer);
    Atomics.store(words, done / 4, status);
    Atomics.notify(words, done / 4);
  }
}

async function shadps4GpuRun(gpu, commands, upload, transient) {
  var device = gpu.device;
  // New shaders and pipelines first: building them is asynchronous, and nothing below may use
  // one before it exists.
  for (var c of commands) {
    if (c.op === "shader") {
      if (Module["shadps4GpuDumpShaders"]) {
        console.log("[webgpu] shader " + c.id + ":\n" + c.code);
      }
      var module = device.createShaderModule({code: c.code});
      gpu.shaders.set(c.id, module);
      shadps4GpuReportCompilation(module, c.id, c.code);
    }
  }
  for (var c of commands) {
    if (c.op === "pipeline") {
      try {
        gpu.pipelines.set(c.id, await shadps4GpuCreatePipeline(gpu, c));
      } catch (e) {
        gpu.pipelines.set(c.id, {error: e.message});
        shadps4GpuLogError("pipeline " + c.id + " (" + c.kind + "): " + e.message);
      }
    }
  }

  var uploadBuffer = null;
  if (upload.byteLength > 0) {
    uploadBuffer = device.createBuffer({
      size: Math.ceil(upload.byteLength / 4) * 4,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.UNIFORM | GPUBufferUsage.INDEX |
             GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST,
    });
    device.queue.writeBuffer(uploadBuffer, 0, upload.buffer, upload.byteOffset,
                             Math.ceil(upload.byteLength / 4) * 4);
    transient.push(uploadBuffer);
  }

  var encoder = device.createCommandEncoder();
  var pass = null;     // the open render pass
  var passKey = null;  // its attachments, to merge consecutive draws into it
  var passTextures = null;
  var copyTargets = null; // textures the draw being bound renders into (sampled as copies)
  var readbacks = [];

  function endPass() {
    if (pass) {
      pass.end();
      pass = null;
      passKey = null;
      passTextures = null;
      finishWritable();
    }
  }

  function textureView(entry) {
    if (entry.dummy) {
      return shadps4GpuDummy(gpu, entry.dummy, entry.dimension, entry.multisampled);
    }
    var t = shadps4GpuTexture(gpu, entry.tex);
    var src = t.texture;
    var view = Object.assign({}, entry.view || {});
    if (entry.depthAsColor) {
      // A depth target read as a color texture: depth formats only bind as depth or
      // unfilterable float, so the value goes through a buffer into an r32float copy.
      var w = t.size[0], h = t.size[1];
      var rowBytes = Math.ceil(w * 4 / 256) * 256;
      var staging = device.createBuffer({size: rowBytes * h,
                                         usage: GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST});
      var copy = device.createTexture({size: [w, h], format: "r32float",
                                       usage: GPUTextureUsage.TEXTURE_BINDING |
                                              GPUTextureUsage.COPY_DST});
      encoder.copyTextureToBuffer({texture: src, aspect: "depth-only"},
                                  {buffer: staging, bytesPerRow: rowBytes}, [w, h]);
      encoder.copyBufferToTexture({buffer: staging, bytesPerRow: rowBytes}, {texture: copy},
                                  [w, h]);
      transient.push(staging, copy);
      src = copy;
      delete view.aspect;
      delete view.format;
    } else if (!entry.storage &&
               (entry.copy || (copyTargets && copyTargets.has(entry.tex)))) {
      // A region of a render target (normalized coordinates span the T#), or the target the
      // draw renders into (WebGPU forbids sampling an attachment): sample a copy.
      var cw = entry.copy ? entry.copy[0] : t.size[0];
      var ch = entry.copy ? entry.copy[1] : t.size[1];
      var copy = device.createTexture({size: [cw, ch, t.size[2]], format: t.format,
                                       viewFormats: t.viewFormats,
                                       usage: GPUTextureUsage.TEXTURE_BINDING |
                                              GPUTextureUsage.COPY_DST});
      encoder.copyTextureToTexture({texture: src}, {texture: copy}, [cw, ch, t.size[2]]);
      transient.push(copy);
      src = copy;
    }
    return src.createView(view);
  }

  function sampler(desc) {
    var key = JSON.stringify(desc);
    var s = gpu.samplers.get(key);
    if (!s) {
      s = device.createSampler(desc);
      gpu.samplers.set(key, s);
    }
    return s;
  }

  // Writable buffers get a GPU buffer of their own: WebGPU tracks usage per buffer, and one
  // that's written can't also be read through another binding in the same pass. Their contents
  // go back into the upload buffer afterwards (finishWritable), where readBuffer and later
  // commands find them.
  var writable = [];
  function finishWritable() {
    for (var w of writable) {
      encoder.copyBufferToBuffer(w.buffer, 0, uploadBuffer, w.offset, w.size);
    }
    writable = [];
  }

  function bindGroup(layout, entries, layoutDescs) {
    return device.createBindGroup({layout: layout, entries: entries.map(function (e, i) {
      if (e.buf) {
        var desc = layoutDescs && layoutDescs.find(function (d) { return d.b === e.b; });
        if (desc && desc.writable) {
          var own = device.createBuffer({size: e.buf[1], usage: GPUBufferUsage.STORAGE |
                                         GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST});
          encoder.copyBufferToBuffer(uploadBuffer, e.buf[0], own, 0, e.buf[1]);
          transient.push(own);
          writable.push({buffer: own, offset: e.buf[0], size: e.buf[1]});
          return {binding: e.b, resource: {buffer: own}};
        }
        return {binding: e.b, resource: {buffer: uploadBuffer, offset: e.buf[0], size: e.buf[1]}};
      }
      if (e.samp) {
        return {binding: e.b, resource: sampler(e.samp)};
      }
      return {binding: e.b, resource: textureView(e)};
    })});
  }

  function beginPass(desc) {
    var key = JSON.stringify(desc);
    var clears = (desc.color || []).some(function (a) { return a && a.load === "clear"; }) ||
                 (desc.depth && (desc.depth.depthLoad === "clear" ||
                                 desc.depth.stencilLoad === "clear"));
    if (pass && passKey === key && !clears) {
      return;
    }
    endPass();
    var textures = new Set();
    var colorAttachments = (desc.color || []).map(function (a) {
      if (!a) {
        return null;
      }
      textures.add(a.tex);
      var t = shadps4GpuTexture(gpu, a.tex);
      return {view: t.texture.createView({dimension: "2d", baseArrayLayer: 0,
                                          arrayLayerCount: 1, mipLevelCount: 1}),
              loadOp: a.load === "clear" ? "clear" : "load", storeOp: "store",
              clearValue: a.clear || [0, 0, 0, 0]};
    });
    var depth;
    if (desc.depth) {
      var d = desc.depth;
      textures.add(d.tex);
      var t = shadps4GpuTexture(gpu, d.tex);
      depth = {view: t.texture.createView({dimension: "2d", baseArrayLayer: 0,
                                           arrayLayerCount: 1, mipLevelCount: 1})};
      if (t.format !== "stencil8") {
        depth.depthLoadOp = d.depthLoad === "clear" ? "clear" : "load";
        depth.depthStoreOp = "store";
        depth.depthClearValue = d.depthClear;
      }
      if (t.format.indexOf("stencil") >= 0) {
        depth.stencilLoadOp = d.stencilLoad === "clear" ? "clear" : "load";
        depth.stencilStoreOp = "store";
        depth.stencilClearValue = d.stencilClear;
      }
    }
    pass = encoder.beginRenderPass({colorAttachments: colorAttachments,
                                    depthStencilAttachment: depth});
    passTextures = textures;
    // Later draws with the same attachments continue this pass; their loads are "load".
    passKey = JSON.stringify({
      color: (desc.color || []).map(function (a) {
        return a ? {tex: a.tex, load: "load"} : null;
      }),
      depth: desc.depth ? {tex: desc.depth.tex, depthLoad: "load", depthClear: desc.depth.depthClear,
                           stencilLoad: "load", stencilClear: desc.depth.stencilClear} : null,
    });
  }

  for (var c of commands) {
    switch (c.op) {
    case "shader":
    case "pipeline":
      break;
    case "texture": {
      endPass();
      var usage = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC |
                  GPUTextureUsage.COPY_DST;
      if (c.renderTarget) {
        usage |= GPUTextureUsage.RENDER_ATTACHMENT;
      }
      if (c.storage) {
        usage |= GPUTextureUsage.STORAGE_BINDING;
      }
      if (c.format.startsWith("depth") || c.format === "stencil8") {
        // Depth formats can't be copied into; depth24plus can't be copied at all.
        usage = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT |
                (c.format.startsWith("depth24") ? 0 : GPUTextureUsage.COPY_SRC);
      }
      var texture = device.createTexture({size: c.size, format: c.format,
                                          dimension: c.dimension, mipLevelCount: c.levels,
                                          viewFormats: c.viewFormats, usage: usage});
      gpu.textures.set(c.id, {texture: texture, format: c.format, size: c.size,
                              viewFormats: c.viewFormats});
      break;
    }
    case "destroyTexture": {
      endPass();
      var t = gpu.textures.get(c.texture);
      if (t) {
        transient.push(t.texture); // destroyed once this batch's work is submitted
        gpu.textures.delete(c.texture);
      }
      break;
    }
    case "write": {
      endPass();
      // Writes go through the encoder (not queue.writeTexture) so they land in order with the
      // draws around them.
      var t = shadps4GpuTexture(gpu, c.texture);
      var rowBytes = c.bytesPerRow;
      var aligned = Math.ceil(rowBytes / 256) * 256;
      var rows = c.rowsPerImage, depth = c.size[2];
      var staging = device.createBuffer({size: aligned * rows * depth,
                                         usage: GPUBufferUsage.COPY_SRC, mappedAtCreation: true});
      var dst = new Uint8Array(staging.getMappedRange());
      for (var z = 0; z < depth; z++) {
        for (var y = 0; y < rows; y++) {
          var from = c.offset + (z * rows + y) * rowBytes;
          dst.set(upload.subarray(from, from + rowBytes), (z * rows + y) * aligned);
        }
      }
      staging.unmap();
      encoder.copyBufferToTexture({buffer: staging, bytesPerRow: aligned, rowsPerImage: rows},
                                  {texture: t.texture, mipLevel: c.level}, c.size);
      transient.push(staging);
      break;
    }
    case "clearTexture": {
      endPass();
      var t = shadps4GpuTexture(gpu, c.texture);
      var view = t.texture.createView({dimension: "2d", baseArrayLayer: 0, arrayLayerCount: 1,
                                       mipLevelCount: 1});
      var desc;
      if (t.format.startsWith("depth") || t.format === "stencil8") {
        var att = {view: view};
        if (t.format !== "stencil8") {
          att.depthLoadOp = "clear";
          att.depthStoreOp = "store";
          att.depthClearValue = c.depth || 0;
        }
        if (t.format.indexOf("stencil") >= 0) {
          att.stencilLoadOp = "clear";
          att.stencilStoreOp = "store";
          att.stencilClearValue = c.stencil || 0;
        }
        desc = {colorAttachments: [], depthStencilAttachment: att};
      } else {
        desc = {colorAttachments: [{view: view, loadOp: "clear", storeOp: "store",
                                    clearValue: c.color || [0, 0, 0, 0]}]};
      }
      encoder.beginRenderPass(desc).end();
      break;
    }
    case "copyTexture": {
      endPass();
      var s = shadps4GpuTexture(gpu, c.src), d = shadps4GpuTexture(gpu, c.dst);
      if (s.format === d.format) {
        encoder.copyTextureToTexture({texture: s.texture}, {texture: d.texture}, c.size);
      }
      break;
    }
    case "draw": {
      var p = gpu.pipelines.get(c.pipeline);
      if (!p || p.error) {
        break;
      }
      // Bind groups first, outside any pass: sampling can need a copy (of a region, of a depth
      // target, or of a target this draw renders into), and copies can't be recorded in a pass.
      // A texture the open pass renders into can't be sampled inside it either.
      var willRenderTo = new Set();
      for (var a of c.pass.color) {
        if (a) willRenderTo.add(a.tex);
      }
      if (c.pass.depth) willRenderTo.add(c.pass.depth.tex);
      var breaksPass = pass && c.groups.some(function (g) {
        return g.some(function (e) {
          return e.tex && (e.copy || e.depthAsColor || willRenderTo.has(e.tex) ||
                           passTextures.has(e.tex));
        });
      });
      if (breaksPass) {
        endPass();
      }
      copyTargets = willRenderTo;
      var hasWritable = c.groups.some(function (g, i) {
        return p.descs[i].some(function (d) { return d.writable; });
      });
      if (hasWritable) {
        endPass();
      }
      var groups = c.groups.map(function (entries, i) {
        return bindGroup(p.layouts[i], entries, p.descs[i]);
      });
      copyTargets = null;
      beginPass(c.pass);
      pass.setPipeline(p.pipeline);
      groups.forEach(function (g, i) { pass.setBindGroup(i, g); });
      pass.setScissorRect(c.scissor[0], c.scissor[1], c.scissor[2], c.scissor[3]);
      pass.setBlendConstant(c.blendConstant);
      pass.setStencilReference(c.stencilReference);
      if (c.index) {
        pass.setIndexBuffer(uploadBuffer, c.index.format, c.index.offset, c.index.size);
        pass.drawIndexed(c.indexCount, c.instances, 0, c.baseVertex, 0);
      } else {
        pass.draw(c.vertexCount, c.instances, c.firstVertex, 0);
      }
      break;
    }
    case "dispatch": {
      endPass();
      var p = gpu.pipelines.get(c.pipeline);
      if (!p || p.error) {
        throw new Error("compute pipeline " + c.pipeline + " failed to build");
      }
      var groups = c.groups.map(function (entries, i) {
        return bindGroup(p.layouts[i], entries, p.descs[i]);
      });
      var cp = encoder.beginComputePass();
      cp.setPipeline(p.pipeline);
      groups.forEach(function (g, i) { cp.setBindGroup(i, g); });
      cp.dispatchWorkgroups(c.count[0], c.count[1], c.count[2]);
      cp.end();
      finishWritable();
      break;
    }
    case "readBuffer": {
      endPass();
      var size = Math.ceil(c.size / 4) * 4;
      var staging = device.createBuffer({size: size,
                                         usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST});
      encoder.copyBufferToBuffer(uploadBuffer, c.offset, staging, 0, size);
      transient.push(staging);
      readbacks.push({staging: staging, ptr: c.ptr, rowBytes: c.size, stride: c.size, rows: 1});
      break;
    }
    case "readTexture": {
      endPass();
      var t = shadps4GpuTexture(gpu, c.texture);
      var aligned = Math.ceil(c.bytesPerRow / 256) * 256;
      var staging = device.createBuffer({size: aligned * c.height,
                                         usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST});
      encoder.copyTextureToBuffer({texture: t.texture}, {buffer: staging, bytesPerRow: aligned},
                                  [c.width, c.height]);
      transient.push(staging);
      readbacks.push({staging: staging, ptr: c.ptr, rowBytes: c.bytesPerRow, stride: aligned,
                      rows: c.height});
      break;
    }
    default:
      throw new Error("unknown command " + c.op);
    }
  }
  endPass();
  device.pushErrorScope("validation");
  device.queue.submit([encoder.finish()]);
  var error = await device.popErrorScope();
  if (error) {
    shadps4GpuLogError("batch: " + error.message);
  }
  if (readbacks.length) {
    await Promise.all(readbacks.map(function (r) { return r.staging.mapAsync(GPUMapMode.READ); }));
    var heap = shadps4GpuHeap();
    for (var r of readbacks) {
      var data = new Uint8Array(r.staging.getMappedRange());
      for (var y = 0; y < r.rows; y++) {
        heap.set(data.subarray(y * r.stride, y * r.stride + r.rowBytes), r.ptr + y * r.rowBytes);
      }
    }
  }
  if (error) {
    throw new Error("batch failed validation");
  }
}
