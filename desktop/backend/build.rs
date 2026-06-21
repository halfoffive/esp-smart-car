fn main() {
    // 前端构建产物目录（相对于 backend）
    let dist_dir = "./frontend/dist";

    // 检查 frontend/dist 是否存在，不存在则 panic 提示用户手动构建前端
    if !std::path::Path::new(dist_dir).exists() {
        panic!("请先构建前端：cd desktop/frontend && bun install && bun run build");
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
