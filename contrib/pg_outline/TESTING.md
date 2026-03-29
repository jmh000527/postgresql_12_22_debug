# pg_outline 测试文档

本目录包含 pg_outline 扩展的综合测试套件。

## 文件说明

- **comprehensive_outline_test.sql** - 主测试文件，包含25种不同类型的SQL查询测试
- **run_outline_tests.sh** - 自动化测试运行脚本
- **COMPREHENSIVE_TEST_CN.md** - 详细的中文测试说明文档
- **test_results/** - 测试结果输出目录（自动创建）

## 快速开始

### 1. 本地测试

```bash
# 最简单的方式
cd contrib/pg_outline
./run_outline_tests.sh
```

### 2. 远程数据库测试

```bash
# 指定连接参数
./run_outline_tests.sh -H remote.server.com -p 5432 -d testdb -u testuser

# 或者使用环境变量
export PGHOST=remote.server.com
export PGPORT=5432
export PGDATABASE=testdb
export PGUSER=testuser
./run_outline_tests.sh
```

### 3. 手动运行测试

```bash
# 使用 psql 直接运行
psql -h localhost -d postgres -f sql/comprehensive_outline_test.sql

# 或在 psql 交互模式中
psql -h localhost -d postgres
\i sql/comprehensive_outline_test.sql
```

## 测试覆盖范围

测试套件包含25个测试用例，覆盖以下SQL类型：

### 基础查询 (3个测试)
- 简单 SELECT 带 WHERE
- 聚合查询
- DISTINCT 查询

### JOIN 查询 (5个测试)
- INNER JOIN
- LEFT JOIN
- 多表 JOIN
- Self JOIN
- CROSS JOIN

### 子查询 (7个测试)
- 标量子查询
- IN 子查询
- EXISTS 子查询
- NOT EXISTS 子查询
- FROM 子句子查询
- ANY/ALL 子查询
- 关联子查询

### CTE 公共表表达式 (3个测试)
- 简单 CTE
- 多个 CTE
- 递归 CTE

### 聚合和窗口函数 (2个测试)
- GROUP BY with HAVING
- 窗口函数

### 集合操作 (4个测试)
- UNION
- UNION ALL
- INTERSECT
- EXCEPT

### 复杂查询 (1个测试)
- 多层嵌套 + JOIN + 子查询 + ORDER BY + LIMIT

## 测试脚本选项

```bash
./run_outline_tests.sh [OPTIONS]

选项：
    -h, --help          显示帮助信息
    -H, --host HOST     PostgreSQL 主机 (默认: localhost)
    -p, --port PORT     PostgreSQL 端口 (默认: 5432)
    -d, --database DB   数据库名称 (默认: postgres)
    -u, --user USER     数据库用户 (默认: postgres)
    -o, --output DIR    输出目录 (默认: ./test_results)
    -v, --verbose       启用详细输出
    -q, --quiet         只显示错误信息
```

## 预期结果

成功运行测试后，你应该看到：

1. **连接确认** - 数据库连接成功
2. **扩展检查** - pg_outline 扩展可用
3. **测试执行** - 所有25个测试用例执行
4. **结果摘要**:
   - 生成的 outline 总数
   - 错误和警告数量
   - 执行时间

### 示例输出

```
============================================================================
Testing Database Connection
============================================================================
  Host: localhost
  Port: 5432
  Database: postgres
  User: postgres
✓ Database connection successful

============================================================================
Checking pg_outline Extension
============================================================================
✓ pg_outline extension is available

============================================================================
Running Comprehensive Tests
============================================================================
  Output will be saved to: ./test_results/test_output_20240329_143022.log
✓ All tests completed successfully
  Duration: 12 seconds

============================================================================
Test Summary
============================================================================
  Total outlines generated: 25
✓ No errors or warnings found

============================================================================
Test Run Complete
============================================================================
✓ All tests passed!
```

## 检查测试结果

### 查看详细输出

```bash
# 查看最新的测试输出
ls -lt test_results/test_output_*.log | head -1 | awk '{print $NF}' | xargs cat

# 或者直接查看
cat test_results/test_output_YYYYMMDD_HHMMSS.log
```

### 查看生成的 Outlines

在测试数据库中：

```sql
-- 查看所有生成的 outlines
SELECT * FROM pg_outline_list();

-- 查看特定查询类型的 outlines
SELECT outline_name, query_pattern, hint_string
FROM pg_outline_data
WHERE query_pattern LIKE '%JOIN%';

-- 检查 SubLink 标签
SELECT outline_name, hint_string
FROM pg_outline_data
WHERE hint_string LIKE '%sublink%';
```

## 故障排查

### 问题: 连接失败

```bash
# 检查 PostgreSQL 是否运行
pg_isready -h localhost -p 5432

# 检查连接权限
psql -h localhost -d postgres -U postgres -c "SELECT 1"
```

### 问题: 扩展未找到

```bash
# 检查扩展是否安装
psql -h localhost -d postgres -c "SELECT * FROM pg_available_extensions WHERE name = 'pg_outline';"

# 如果未安装，需要先编译和安装
cd /path/to/postgresql/contrib/pg_outline
make && make install
```

### 问题: 测试失败

```bash
# 启用详细输出
./run_outline_tests.sh -v

# 查看完整错误日志
tail -100 test_results/test_output_*.log
```

### 问题: 权限错误

```bash
# 确保用户有创建扩展的权限
psql -h localhost -d postgres -c "ALTER USER testuser CREATEDB;"

# 或使用超级用户
./run_outline_tests.sh -u postgres
```

## 性能测试

如果需要进行性能测试：

```bash
# 1. 修改测试数据规模
# 编辑 sql/comprehensive_outline_test.sql
# 增加 generate_series 的范围，例如：
# FROM generate_series(1, 100000) i

# 2. 使用 EXPLAIN ANALYZE 代替 EXPLAIN
# 修改每个测试的 EXPLAIN 为 EXPLAIN ANALYZE

# 3. 多次运行取平均值
for i in {1..5}; do
    ./run_outline_tests.sh -q
    echo "Run $i completed"
done
```

## 扩展测试

你可以根据实际需求添加更多测试：

1. **编辑测试文件**:
   ```bash
   vim sql/comprehensive_outline_test.sql
   ```

2. **添加新测试**:
   ```sql
   \echo '============================================================================'
   \echo 'Test 26: Your Custom Test'
   \echo '============================================================================'

   EXPLAIN SELECT ... your query ...;
   ```

3. **运行测试**:
   ```bash
   ./run_outline_tests.sh
   ```

## CI/CD 集成

将测试集成到 CI/CD 流程：

```yaml
# GitHub Actions 示例
- name: Run pg_outline tests
  run: |
    cd contrib/pg_outline
    ./run_outline_tests.sh -H localhost -d testdb -u postgres
```

## 相关文档

- [COMPREHENSIVE_TEST_CN.md](COMPREHENSIVE_TEST_CN.md) - 详细测试说明
- [README.md](README.md) - pg_outline 扩展说明
- [FINGERPRINT_FIX_CN.md](FINGERPRINT_FIX_CN.md) - Fingerprint 一致性修复说明

## 贡献

如果你发现测试问题或想添加新的测试用例，欢迎提交 PR！

## 许可证

与 PostgreSQL 相同的许可证。
