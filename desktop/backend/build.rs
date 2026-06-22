use std::path::Path;
use std::process::Command;

/// 执行命令并返回是否成功 + stdout/stderr（用于错误输出）
fn run_command(program: &str, args: &[&str], cwd: &Path) -> Result<String, String> {
    let output = Command::new(program)
        .args(args)
        .current_dir(cwd)
        .output()
        .map_err(|e| format!("无法执行 {}: {}", program, e))?;

    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&output.stderr).into_owned();

    if output.status.success() {
        Ok(stdout)
    } else {
        let mut msg = format!("{}{}", stdout, stderr);
        if msg.trim().is_empty() {
            msg = format!("{} 返回退出码 {:?}", program, output.status.code());
        }
        Err(msg)
    }
}

/// 检测包管理器是否可用（执行 --version 看退出码）
fn detect_package_manager() -> Option<&'static str> {
    // 优先 bun（项目使用 bun）
    if Command::new("bun").arg("--version").output().map(|o| o.status.success()).unwrap_or(false) {
        return Some("bun");
    }
    // 回退 npm
    if Command::new("npm").arg("--version").output().map(|o| o.status.success()).unwrap_or(false) {
        return Some("npm");
    }
    None
}

fn main() {
    let frontend_dir = Path::new("../frontend");
    let dist_dir = Path::new("./frontend/dist");

    // ============================================================
    // 快速路径：frontend/dist 已存在 → 跳过构建
    // ============================================================
    if dist_dir.join("index.html").exists() {
        eprintln!("[build.rs] frontend/dist 已存在，跳过前端构建");
        emit_rerun_signals(frontend_dir);
        return;
    }

    // ============================================================
    // SKIP_FRONTEND_BUILD 环境变量：跳过构建（CI / 离线场景）
    // ============================================================
    if std::env::var("SKIP_FRONTEND_BUILD").map(|v| v == "1" || v == "true").unwrap_or(false) {
        eprintln!("[build.rs] SKIP_FRONTEND_BUILD=1，跳过前端构建");
        // 不调用 emit_rerun_signals —— 前端是预先手动构建的，不需要监听
        return;
    }

    // ============================================================
    // 自动构建前端
    // ============================================================
    let pm = detect_package_manager().unwrap_or_else(|| {
        panic!(
            "未检测到 bun 或 npm。请安装其中之一后重试：\n\
             - Bun: https://bun.sh\n\
             - Node.js (含 npm): https://nodejs.org\n\n\
             或手动构建前端后设置 SKIP_FRONTEND_BUILD=1：\n\
             cd desktop/frontend && bun install && bun run build"
        );
    });

    eprintln!("[build.rs] 使用 {} 构建前端...", pm);

    // 检查 node_modules 是否存在
    let node_modules = frontend_dir.join("node_modules");
    if !node_modules.exists() {
        eprintln!("[build.rs] 安装依赖 ({} install)...", pm);
        match run_command(pm, &["install"], frontend_dir) {
            Ok(_) => eprintln!("[build.rs] 依赖安装完成"),
            Err(e) => panic!("依赖安装失败:\n{}\n\n请手动执行: cd desktop/frontend && {} install", e, pm),
        }
    }

    // 执行构建
    eprintln!("[build.rs] 构建前端 ({} run build)...", pm);
    match run_command(pm, &["run", "build"], frontend_dir) {
        Ok(_) => eprintln!("[build.rs] 前端构建完成"),
        Err(e) => panic!(
            "前端构建失败:\n{}\n\n请手动排查: cd desktop/frontend && {} run build",
            e, pm
        ),
    }

    // 验证产物
    if !dist_dir.join("index.html").exists() {
        panic!(
            "前端构建完成但未生成 frontend/dist/index.html，请检查 vite.config.ts 的 build.outDir 配置"
        );
    }

    // 监听前端源码变化（开发时触发重新编译）
    emit_rerun_signals(frontend_dir);
}

/// 输出 cargo:rerun-if-changed 指令，监听前端源码变化
fn emit_rerun_signals(frontend_dir: &Path) {
    println!("cargo:rerun-if-changed=../frontend/src");
    println!("cargo:rerun-if-changed=../frontend/index.html");
    println!("cargo:rerun-if-changed=../frontend/package.json");
    println!("cargo:rerun-if-changed=../frontend/vite.config.ts");
    println!("cargo:rerun-if-changed=../frontend/tsconfig.json");
    println!("cargo:rerun-if-changed=../frontend/tsconfig.node.json");
    println!("cargo:rerun-if-changed=../frontend/public");
    // 让编译器知道我们引用了 frontend_dir（消除 unused 警告）
    let _ = frontend_dir;
}
