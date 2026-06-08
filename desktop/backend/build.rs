use std::process::Command;

fn main() {
    // 前端目录（相对于 backend 的父目录）
    let frontend_dir = "../frontend";
    let dist_dir = "./frontend/dist";

    // 检查 frontend/dist 是否存在
    let dist_exists = std::path::Path::new(dist_dir).exists();

    // 检查前端源码是否比构建产物新
    let need_rebuild = if dist_exists {
        check_src_newer_than_dist(frontend_dir, dist_dir)
    } else {
        true
    };

    if need_rebuild {
        println!("cargo:warning=前端构建产物不存在或已过期，正在自动构建前端...");

        // 检查 bun 是否可用
        let bun_check = Command::new("bun")
            .arg("--version")
            .output();

        if bun_check.is_err() {
            panic!("未找到 bun，无法自动构建前端。请手动运行: cd ../frontend && bun install && bun run build");
        }

        // 安装依赖
        let install_result = Command::new("bun")
            .args(["install"])
            .current_dir(frontend_dir)
            .status();

        if let Err(e) = install_result {
            panic!("前端依赖安装失败: {}", e);
        }

        // 检查 node_modules 是否存在，确保依赖已安装
        if !std::path::Path::new(&format!("{}/node_modules", frontend_dir)).exists() {
            panic!("前端依赖安装后 node_modules 目录不存在，请手动运行: cd ../frontend && bun install");
        }

        // 构建前端
        let build_result = Command::new("bun")
            .args(["run", "build"])
            .current_dir(frontend_dir)
            .status();

        match build_result {
            Ok(status) if status.success() => {
                println!("cargo:warning=前端构建成功");
            }
            Ok(status) => {
                panic!("前端构建失败，退出码: {:?}", status.code());
            }
            Err(e) => {
                panic!("前端构建执行失败: {}", e);
            }
        }
    }

    // 监听前端源码变化（用于开发时触发重新构建）
    println!("cargo:rerun-if-changed=../frontend/src");
    println!("cargo:rerun-if-changed=../frontend/index.html");
    println!("cargo:rerun-if-changed=../frontend/package.json");
    println!("cargo:rerun-if-changed=../frontend/vite.config.ts");
    println!("cargo:rerun-if-changed=../frontend/tsconfig.json");
    println!("cargo:rerun-if-changed=../frontend/tsconfig.node.json");
    println!("cargo:rerun-if-changed=../frontend/public");
}

/// 检查前端源码是否比 dist 目录新
fn check_src_newer_than_dist(src_dir: &str, dist_dir: &str) -> bool {
    let dist_mtime = match std::fs::metadata(dist_dir)
        .and_then(|m| m.modified()) {
        Ok(t) => t,
        Err(_) => return true,
    };

    // 递归检查 src 目录下的文件
    fn check_dir(dir: &std::path::Path, dist_mtime: &std::time::SystemTime) -> bool {
        if let Ok(entries) = std::fs::read_dir(dir) {
            for entry in entries.flatten() {
                let path = entry.path();
                if path.is_dir() {
                    if check_dir(&path, dist_mtime) {
                        return true;
                    }
                } else if let Ok(metadata) = entry.metadata() {
                    if let Ok(mtime) = metadata.modified() {
                        if mtime > *dist_mtime {
                            return true;
                        }
                    }
                }
            }
        }
        false
    }

    let src_path = std::path::Path::new(src_dir);
    check_dir(src_path, &dist_mtime)
}
