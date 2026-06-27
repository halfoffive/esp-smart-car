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
    if Command::new("bun")
        .arg("--version")
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
    {
        return Some("bun");
    }
    if Command::new("npm")
        .arg("--version")
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
    {
        return Some("npm");
    }
    None
}

/// 检查是否需要重新构建前端：比较源文件修改时间与 dist/index.html
fn needs_rebuild(frontend_dir: &Path, dist_dir: &Path) -> bool {
    let dist_index = dist_dir.join("index.html");
    let dist_mtime = match std::fs::metadata(&dist_index).and_then(|m| m.modified()) {
        Ok(t) => t,
        Err(_) => return true,
    };

    let key_files = [
        "package.json",
        "index.html",
        "vite.config.ts",
        "tsconfig.json",
        "tsconfig.node.json",
    ];
    for f in &key_files {
        let p = frontend_dir.join(f);
        if let Ok(meta) = std::fs::metadata(&p) {
            if let Ok(mtime) = meta.modified() {
                if mtime > dist_mtime {
                    eprintln!("[build.rs] {} 比 dist 新，需要重新构建", f);
                    return true;
                }
            }
        }
    }

    let src_dir = frontend_dir.join("src");
    if dir_has_newer_files(&src_dir, dist_mtime) {
        eprintln!("[build.rs] src/ 中有文件比 dist 新，需要重新构建");
        return true;
    }

    false
}

/// 递归检查目录中是否有文件比基准时间新
fn dir_has_newer_files(dir: &Path, baseline: std::time::SystemTime) -> bool {
    let entries = match std::fs::read_dir(dir) {
        Ok(e) => e,
        Err(_) => return false,
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if let Ok(meta) = std::fs::metadata(&path) {
            if meta.is_dir() {
                if dir_has_newer_files(&path, baseline) {
                    return true;
                }
            } else if let Ok(mtime) = meta.modified() {
                if mtime > baseline {
                    return true;
                }
            }
        }
    }
    false
}

fn main() {
    let frontend_dir = Path::new("../frontend");
    let dist_dir = Path::new("./frontend/dist");

    if std::env::var("SKIP_FRONTEND_BUILD")
        .map(|v| v == "1" || v == "true")
        .unwrap_or(false)
    {
        eprintln!("[build.rs] SKIP_FRONTEND_BUILD=1，跳过前端构建");
        return;
    }

    if dist_dir.join("index.html").exists() && !needs_rebuild(frontend_dir, dist_dir) {
        eprintln!("[build.rs] frontend/dist 已是最新，跳过前端构建");
        emit_rerun_signals(frontend_dir);
        return;
    }

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

    eprintln!("[build.rs] 检查/安装依赖 ({} install)...", pm);
    match run_command(pm, &["install"], frontend_dir) {
        Ok(_) => eprintln!("[build.rs] 依赖已就绪"),
        Err(e) => panic!(
            "依赖安装失败:\n{}\n\n请手动执行: cd desktop/frontend && {} install",
            e, pm
        ),
    }

    eprintln!("[build.rs] 构建前端 ({} run build)...", pm);
    match run_command(pm, &["run", "build"], frontend_dir) {
        Ok(_) => eprintln!("[build.rs] 前端构建完成"),
        Err(e) => panic!(
            "前端构建失败:\n{}\n\n请手动排查: cd desktop/frontend && {} run build",
            e, pm
        ),
    }

    if !dist_dir.join("index.html").exists() {
        panic!(
            "前端构建完成但未生成 frontend/dist/index.html，请检查 vite.config.ts 的 build.outDir 配置"
        );
    }

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
    let _ = frontend_dir;
}
